#include "board.h"
#include "display.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>

// ===================
// 1. GLOBAIS E ESTADO

session_t *sessions = NULL; // Active sessions
int max_games = 0;
connection_buffer_t conn_buffer; // Para gerir pedidos de comunicação
_Atomic int server_running = 1;
_Atomic int sigusr1_received = 0;
pthread_mutex_t top5_mutex = PTHREAD_MUTEX_INITIALIZER; // Protege geração do top5
char registry_pipe[MAX_PIPE_PATH_LENGTH];
char levels_dir[256];
int shutdown_pipe[2]; 

// Forward declarations (necessário para funções que se chamam mutuamente)
void send_board_update(session_t *sess);
int load_next_level(session_t *sess);
void* update_sender(void* arg);

// ===============================================
// 2. GESTÃO DE BUFFER DE CONEXÃO (Infraestrutura)

void init_connection_buffer(connection_buffer_t *buffer) {
    buffer->in = 0; buffer->out = 0; buffer->active = 1;
    
    sem_unlink("/pacmanist_empty"); sem_unlink("/pacmanist_full");
    buffer->empty = sem_open("/pacmanist_empty", O_CREAT | O_EXCL, 0644, BUFFER_SIZE);
    buffer->full = sem_open("/pacmanist_full", O_CREAT | O_EXCL, 0644, 0);
    
    if (buffer->empty == SEM_FAILED || buffer->full == SEM_FAILED) { 
        perror("sem_open failed"); exit(1); 
    }
    pthread_mutex_init(&buffer->mutex, NULL);
}

// Apenas sinaliza paragem 
void destroy_connection_buffer(connection_buffer_t *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->active = 0;
    pthread_mutex_unlock(&buffer->mutex);
    // Acorda threads bloqueadas no sem_wait
    if (buffer->empty != SEM_FAILED) sem_post(buffer->empty);
    if (buffer->full != SEM_FAILED) sem_post(buffer->full);
}

// Destrói recursos 
void cleanup_connection_resources(connection_buffer_t *buffer) {
    if (buffer->empty != SEM_FAILED) { sem_close(buffer->empty); sem_unlink("/pacmanist_empty"); }
    if (buffer->full != SEM_FAILED) { sem_close(buffer->full); sem_unlink("/pacmanist_full"); }
    pthread_mutex_destroy(&buffer->mutex);
}

void buffer_insert(connection_buffer_t *buffer, connection_request_t *request) {
    // Verificação rápida antes de esperar pelo semáforo
    pthread_mutex_lock(&buffer->mutex);
    if (!buffer->active) { pthread_mutex_unlock(&buffer->mutex); return; }
    pthread_mutex_unlock(&buffer->mutex);

    if (sem_wait(buffer->empty) != 0) return; // Wait por espaço livre

    pthread_mutex_lock(&buffer->mutex);
    if (buffer->active) {
        buffer->requests[buffer->in] = *request;
        buffer->in = (buffer->in + 1) % BUFFER_SIZE;
        sem_post(buffer->full); // Sinaliza item disponível
    } else {
        sem_post(buffer->empty); // Devolve o token se estiver a encerrar
    }
    pthread_mutex_unlock(&buffer->mutex);
}

// Garante que só se consome pedidos quando o buffer está ativo e há pedidos disponíveis
int buffer_remove(connection_buffer_t *buffer, connection_request_t *request) {
    // Se destroy_connection_buffer for chamado, ele faz post no semáforo, acordando esta thread
    if (sem_wait(buffer->full) != 0) return -1; // Espera por item disponível

    pthread_mutex_lock(&buffer->mutex);
    if (buffer->active) {
        *request = buffer->requests[buffer->out];
        buffer->out = (buffer->out + 1) % BUFFER_SIZE;
        sem_post(buffer->empty); // Sinaliza espaço livre
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }
    
    // Se acordou mas o buffer não está ativo (shutdown)
    sem_post(buffer->full); // Repõe o token para outras threads acordarem e saírem
    pthread_mutex_unlock(&buffer->mutex);
    return -1;
}

// =========================================
// 3. NÍVEIS, RECURSOS E PONTUAÇÃO (Helpers)

// Liberta recursos internos de uma sessão (pipes, memória do tabuleiro)
void free_session_resources(session_t *sess) {
    if (sess->req_fd != -1) { close(sess->req_fd); sess->req_fd = -1; }
    if (sess->notif_fd != -1) { close(sess->notif_fd); sess->notif_fd = -1; }
    if (sess->board != NULL) {
        unload_level(sess->board);
        free(sess->board);
        sess->board = NULL;
    }
    sess->game_active = 0; 
}

// Níveis e ficheiros
int load_next_level(session_t *sess) {
    DIR* level_dir = opendir(levels_dir);
    if (!level_dir) {
        debug("Session %d: Failed to open levels directory\n", sess->session_id);
        return -1;
    }

    char level_files[100][256];
    int num_levels = 0;
    struct dirent* entry;

    while ((entry = readdir(level_dir)) != NULL && num_levels < 100) {
        if (entry->d_name[0] == '.') continue;
        // Otimização: Verificação simples de extensão antes de processar
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".lvl") == 0) {
            strncpy(level_files[num_levels], entry->d_name, 255);
            level_files[num_levels][255] = '\0';
            num_levels++;
        }
    }
    closedir(level_dir);

    debug("Session %d: Found %d levels, current: %d\n", sess->session_id, num_levels, sess->current_level);

    if (sess->current_level >= num_levels) return -1; // Sem mais níveis

    // Preservar pontos
    int accumulated_points = 0;
    if (sess->board && sess->board->n_pacmans > 0) accumulated_points = sess->board->pacmans[0].points;

    // Recarregar board
    if (sess->board) unload_level(sess->board);
    else {
        sess->board = malloc(sizeof(board_t));
        if (!sess->board) {
            debug("Session %d: Failed to allocate board memory\n", sess->session_id);
            return -1;
        }
    }

    debug("Session %d: Loading %s\n", sess->session_id, level_files[sess->current_level]);
    if (load_level(sess->board, level_files[sess->current_level], levels_dir, 0) != 0) return -1;

    // Restaurar pontos
    if (sess->board->n_pacmans > 0) sess->board->pacmans[0].points = accumulated_points;
    return 0;
}

// Estrutura auxiliar e função de comparação para qsort
struct score_entry { int id; int pts; };

int compare_scores(const void *a, const void *b) {
    // Ordem decrescente de pontos
    return ((struct score_entry*)b)->pts - ((struct score_entry*)a)->pts;
}

void generate_top5_file() {
    // Mutex para evitar múltiplas execuções simultâneas
    if (pthread_mutex_trylock(&top5_mutex) != 0) {
        debug("Top 5 generation already in progress, skipping...\n");
        return; // Já está a gerar, ignora este pedido
    }
    
    debug("Generating top 5 clients file...\n");
    struct score_entry scores[max_games];
    int num = 0;

    for (int i = 0; i < max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        if (sessions[i].active && sessions[i].board) {
            scores[num].id = sessions[i].session_id;
            scores[num].pts = (sessions[i].board->n_pacmans > 0) ? sessions[i].board->pacmans[0].points : 0;
            num++;
        }
        pthread_mutex_unlock(&sessions[i].session_lock);
    }

    // Otimização: Uso de qsort (Quick Sort) em vez de Bubble Sort
    if (num > 1) {
        qsort(scores, num, sizeof(struct score_entry), compare_scores);
    }

    // Escrever o ficheiro
    FILE *f = fopen("top5.txt", "w");
    if (f) {
        fprintf(f, "Top 5 Clientes por Pontuação\n================================\n\n");
        int limit = (num < 5) ? num : 5;
        for (int i = 0; i < limit; i++) fprintf(f, "%d. Cliente ID %d: %d pontos\n", i + 1, scores[i].id, scores[i].pts);
        if (num == 0) fprintf(f, "Nenhum cliente ativo.\n");
        fclose(f);
        debug("Top 5 generated with %d clients\n", limit);
    } else {
        debug("Failed to open top5.txt for writing\n");
    }
    
    pthread_mutex_unlock(&top5_mutex);
}

// ==========================
// LÓGICA DO JOGO E PROTOCOLO

// Envia update do board para o cliente
void send_board_update(session_t *sess) {
    if (!sess->board || sess->notif_fd == -1) return;
    board_t *b = sess->board;
    
    // Otimização: Buffer maior para evitar overflow em mapas grandes (ex: 100x100)
    char msg[16384]; 
    int off = 0;

    msg[off++] = OP_CODE_BOARD;

    memcpy(msg + off, &b->width, sizeof(int)); off += sizeof(int);
    memcpy(msg + off, &b->height, sizeof(int)); off += sizeof(int);
    memcpy(msg + off, &b->tempo, sizeof(int)); off += sizeof(int);
    memcpy(msg + off, &sess->victory, sizeof(int)); off += sizeof(int);
    
    int game_over_val = (b->n_pacmans > 0 && !b->pacmans[0].alive) ? 1 : 0;
    memcpy(msg + off, &game_over_val, sizeof(int)); off += sizeof(int);
    
    int points_val = (b->n_pacmans > 0) ? b->pacmans[0].points : 0;
    memcpy(msg + off, &points_val, sizeof(int)); off += sizeof(int);

    int total_cells = b->width * b->height;
    // Otimização: Switch é geralmente mais eficiente que múltiplos if-else e evita verificações repetidas
    for (int i = 0; i < total_cells; i++) {
        char ch = b->board[i].content;
        switch(ch) {
            case 'W': msg[off++] = '#'; break;
            case 'P': msg[off++] = 'C'; break;
            case 'M': msg[off++] = 'M'; break;
            default:
                if (b->board[i].has_portal) msg[off++] = '@';
                else if (b->board[i].has_dot) msg[off++] = '.';
                else msg[off++] = ' ';
                break;
        }
    }
    
    if (write(sess->notif_fd, msg, off) == -1) {}
}

// Retorna 1 se o jogo continua, 0 se o jogo deve parar
// Retorna 2 se passou de nível (requer restart da update_thread)
int handle_move_result(session_t *sess, int result) {
    if (result == REACHED_PORTAL) {
        debug("Session %d: Pacman reached portal!\n", sess->session_id);
        
        pthread_mutex_lock(&sess->session_lock);
        sess->current_level++;
        
        if (load_next_level(sess) != 0) {
            sess->victory = 1;
            sess->game_active = 0;
            send_board_update(sess);
            pthread_mutex_unlock(&sess->session_lock);
            return 0; 
        }
        
        sess->game_active = 1;  // Reativa o jogo
        send_board_update(sess);
        pthread_mutex_unlock(&sess->session_lock);
        
        // Restart update_sender thread
        if (pthread_create(&sess->update_thread, NULL, update_sender, sess) != 0) {
            debug("Session %d: Failed to restart update thread\n", sess->session_id);
            pthread_mutex_lock(&sess->session_lock);
            sess->game_active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            return 0;
        }
        
        return 2; // Indica que passou de nível
    } 
    
    if (result == DEAD_PACMAN) {
        debug("Session %d: Pacman died!\n", sess->session_id);
        pthread_mutex_lock(&sess->session_lock);
        sess->game_active = 0;
        send_board_update(sess);
        pthread_mutex_unlock(&sess->session_lock);
        return 0;
    }
    
    return 1;
}

// ============================
// THREADS (Lógica de Execução)

void* update_sender(void* arg) {
    session_t *sess = (session_t*)arg;
    int keep_running = 1;
    while (keep_running) {
        pthread_mutex_lock(&sess->session_lock);
        keep_running = sess->game_active;
        if (!sess->game_active || !sess->board) { pthread_mutex_unlock(&sess->session_lock); break; }
        int tempo = sess->board->tempo;
        pthread_mutex_unlock(&sess->session_lock);

        sleep_ms(tempo);

        pthread_mutex_lock(&sess->session_lock);
        if (sess->game_active && sess->board) {
            board_t *b = sess->board;
            pthread_rwlock_wrlock(&b->state_lock);
            
            // Movimento Monstros
            for (int i = 0; i < b->n_ghosts; i++) {
                if (b->ghosts[i].n_moves > 0)
                    move_ghost(b, i, &b->ghosts[i].moves[b->ghosts[i].current_move % b->ghosts[i].n_moves]);
            }
            
            pthread_rwlock_unlock(&b->state_lock);
            
            if (sess->game_active && sess->board) send_board_update(sess);
        }
        pthread_mutex_unlock(&sess->session_lock);
    }
    return NULL;
}

// Gere toda a vida de uma sessão de jogo
void* session_handler(void* arg) {
    session_t *sess = (session_t*)arg;
    debug("Session %d handler started\n", sess->session_id);
    
    if (sess->req_fd == -1 || sess->notif_fd == -1) {
        debug("Session %d: Pipes not properly opened\n", sess->session_id);
        pthread_mutex_lock(&sess->session_lock);
        sess->active = 0;
        pthread_mutex_unlock(&sess->session_lock);
        return NULL;
    }

    pthread_mutex_lock(&sess->session_lock);
    send_board_update(sess);
    pthread_mutex_unlock(&sess->session_lock);
    
    if (pthread_create(&sess->update_thread, NULL, update_sender, sess) != 0) {
        debug("Session %d: Failed to create update thread\n", sess->session_id);
        sess->game_active = 0;
        close(sess->req_fd);
        close(sess->notif_fd);
        sess->active = 0;
        return NULL;
    }

    char buf[256];
    int keep_running = 1;
    int update_thread_joined = 0;

    while (keep_running && server_running) { // Verificar server_running
        pthread_mutex_lock(&sess->session_lock);
        keep_running = sess->game_active;
        pthread_mutex_unlock(&sess->session_lock);
        
        if (!keep_running) break;
        
        // --- ALTERAÇÃO: Usar select com timeout em vez de read bloqueante ---
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sess->req_fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms timeout

        int retval = select(sess->req_fd + 1, &read_fds, NULL, NULL, &tv);

        if (!server_running) break; // Verificar shutdown após o select

        if (retval == -1) {
            if (errno == EINTR) continue; // Sinal recebido, tentar de novo
            debug("Session %d: Select error\n", sess->session_id);
            break;
        } else if (retval == 0) {
            continue; // Timeout, volta ao início do loop para verificar flags
        }

        // Se chegámos aqui, há dados para ler
        if (read(sess->req_fd, buf, sizeof(buf)) <= 0) {
            debug("Client disconnected or error\n");
            break; 
        }
        // -------------------------------------------------------------------

        pthread_mutex_lock(&sess->session_lock);
        if (buf[0] == OP_CODE_DISCONNECT) {
            sess->game_active = 0;
            char resp[] = { OP_CODE_DISCONNECT, 0 };
            if (write(sess->notif_fd, resp, 2) == -1) {}
            pthread_mutex_unlock(&sess->session_lock);

        } else if (buf[0] == OP_CODE_PLAY && sess->board && sess->board->n_pacmans > 0) {
            command_t cmd = { .command = buf[1], .turns = 1, .turns_left = 1 };
            board_t *current_board = sess->board;
            pthread_mutex_unlock(&sess->session_lock);
            
            pthread_rwlock_wrlock(&current_board->state_lock);
            int res = move_pacman(current_board, 0, &cmd);
            pthread_rwlock_unlock(&current_board->state_lock);
            
            if (res == REACHED_PORTAL) {
                pthread_mutex_lock(&sess->session_lock);
                sess->game_active = 0;
                pthread_mutex_unlock(&sess->session_lock);
                
                pthread_join(sess->update_thread, NULL);
                update_thread_joined = 1;
                
                int move_result = handle_move_result(sess, res);
                if (move_result == 2) {
                    update_thread_joined = 0;
                } else if (move_result == 0) {
                    break;
                }
                continue;
            }
            
            int move_result = handle_move_result(sess, res);
            if (move_result == 1) {
                pthread_mutex_lock(&sess->session_lock);
                send_board_update(sess);
                pthread_mutex_unlock(&sess->session_lock);
            } else if (move_result == 0) {
                break;
            }
            continue;
        } else {
            debug("Session %d: Unknown opcode %d - ignoring\n", sess->session_id, buf[0]);
            pthread_mutex_unlock(&sess->session_lock);
        }
    }

    pthread_mutex_lock(&sess->session_lock);
    sess->game_active = 0;
    pthread_mutex_unlock(&sess->session_lock);
    
    if (!update_thread_joined) {
        pthread_join(sess->update_thread, NULL);
    }
    free_session_resources(sess);

    int local_id = sess->session_id;

    pthread_mutex_lock(&sess->session_lock);
    sess->active = 0; 
    pthread_mutex_unlock(&sess->session_lock);
    
    debug("Session %d ended (Slot freed)\n", local_id);
    
    return NULL;
}

// Aceita novos clientes e cria sessões de jogo
void* manager_thread(void* arg) {
    int id = *(int*)arg; free(arg);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1); pthread_sigmask(SIG_BLOCK, &mask, NULL);
    debug("Manager %d started\n", id);

    while (server_running) {
        connection_request_t req;
        if (buffer_remove(&conn_buffer, &req) != 0) {
            if (!server_running) break;
            continue;
        }

        // Extrair o ID do cliente ANTES de procurar slot
        char *filename = strrchr(req.req_pipe_path, '/');
        if (filename) filename++; else filename = req.req_pipe_path;
        int requested_id = atoi(filename);

        // Percorre todas as sessões para ver se este ID já está ativo
        int is_duplicate = 0;
        for (int i = 0; i < max_games; i++) {
            pthread_mutex_lock(&sessions[i].session_lock);
            if (sessions[i].active && sessions[i].session_id == requested_id) {
                is_duplicate = 1;
            }
            pthread_mutex_unlock(&sessions[i].session_lock);
            
            if (is_duplicate) break; 
        }

        if (is_duplicate) {
            debug("Manager %d: Client ID %d already active. Ignoring request.\n", id, requested_id);
            // Ignora o pedido e volta ao início do loop
            continue;
        }

        int sess_id = -1;
        // Procura slot livre
        for (int i = 0; i < max_games; i++) {
            pthread_mutex_lock(&sessions[i].session_lock);
            if (!sessions[i].active) {
                sess_id = i; 
                sessions[i].active = 1; 
                
                // Atribuição do ID (que já extraímos em cima)
                sessions[i].session_id = requested_id;
                
                sessions[i].game_active = 1; 
                sessions[i].victory = 0; 
                sessions[i].current_level = 0;
                strncpy(sessions[i].req_pipe_path, req.req_pipe_path, MAX_PIPE_PATH_LENGTH);
                strncpy(sessions[i].notif_pipe_path, req.notif_pipe_path, MAX_PIPE_PATH_LENGTH);
                
                pthread_mutex_unlock(&sessions[i].session_lock);
                break;
            }
            pthread_mutex_unlock(&sessions[i].session_lock);
        }

        if (sess_id == -1) { 
            debug("Manager %d: No slots available, putting request back\n", id);
            // Colocar pedido de volta no buffer para reprocessar quando houver slot
            buffer_insert(&conn_buffer, &req);
            sleep(1); // Esperar 1 segundo antes de tentar novamente
            continue;
        }
        
        session_t *sess = &sessions[sess_id];
        
        // Abrir pipes ANTES de enviar confirmação (evita deadlock)
        sess->req_fd = open(req.req_pipe_path, O_RDONLY);
        if (sess->req_fd == -1) {
            debug("Manager %d: Failed to open req_pipe\n", id);
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        sess->notif_fd = open(req.notif_pipe_path, O_WRONLY);
        if (sess->notif_fd == -1) {
            debug("Manager %d: Failed to open notif_pipe\n", id);
            close(sess->req_fd);
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        // Agora enviar mensagem de confirmação ao cliente
        char confirm_msg[2] = { OP_CODE_CONNECT, 0 };
        if (write(sess->notif_fd, confirm_msg, 2) == -1) {
            perror("Failed to send confirmation");
            close(sess->req_fd);
            close(sess->notif_fd);
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        sess->board = NULL;
        
        pthread_mutex_lock(&sess->session_lock);
        int level_loaded = (load_next_level(sess) == 0);
        pthread_mutex_unlock(&sess->session_lock);
        
        if (!level_loaded) {
            close(sess->req_fd);
            close(sess->notif_fd);
            pthread_mutex_lock(&sess->session_lock); 
            sess->active = 0; 
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        session_handler(sess);
    }
    debug("Manager %d ended\n", id);
    return NULL;
}

// Recebe e prepara um novo pedido de ligação de cliente
void* host_thread(void* arg) {
    (void)arg;
    debug("Host thread started\n");
    
    int reg_fd = open(registry_pipe, O_RDWR | O_NONBLOCK);
    if (reg_fd == -1) { debug("Failed to open registry\n"); return NULL; }

    while (server_running) {
        // Verifica se recebeu SIGUSR1
        if (sigusr1_received) { 
            sigusr1_received = 0; 
            generate_top5_file(); 
        }
        
        // Usa select com timeout para não bloquear indefinidamente
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(reg_fd, &readfds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000; // 500ms timeout
        
        int ready = select(reg_fd + 1, &readfds, NULL, NULL, &timeout);
        
        if (!server_running) break;
        
        if (ready < 0) {
            if (errno == EINTR) continue; // Sinal recebido, volta ao loop
            sleep_ms(100);
            continue;
        }
        
        if (ready == 0) continue; // Timeout, volta ao loop para verificar sigusr1_received
        
        // Há dados para ler
        char buf[1 + MAX_PIPE_PATH_LENGTH * 3];
        ssize_t n = read(reg_fd, buf, sizeof(buf));
        
        if (n <= 0) {
            if (n == 0) { // EOF - reabrir
                close(reg_fd);
                reg_fd = open(registry_pipe, O_RDWR | O_NONBLOCK);
                if (reg_fd == -1) sleep_ms(100);
            } else if (errno != EINTR) {
                sleep_ms(100);
            }
            continue;
        }

        if (buf[0] == OP_CODE_CONNECT) {
            connection_request_t req;
            memcpy(req.req_pipe_path, buf + 1, MAX_PIPE_PATH_LENGTH);
            memcpy(req.notif_pipe_path, buf + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
            req.req_pipe_path[MAX_PIPE_PATH_LENGTH-1] = req.notif_pipe_path[MAX_PIPE_PATH_LENGTH-1] = '\0';
            
            debug("Connect req: %s\n", req.req_pipe_path);
            buffer_insert(&conn_buffer, &req);
        }
    }
    close(reg_fd);
    debug("Host thread ended\n");
    return NULL;
}

// ==================================================
// MAIN E GESTÃO DE SINAIS (Ponto de Entrada e Saída)

void signal_handler(int signum) {
    if (signum == SIGINT) {
        server_running = 0;
        char c = 1; 
        if (write(shutdown_pipe[1], &c, 1) == -1) {} // Ignorar erro
    } else if (signum == SIGUSR1) {
        sigusr1_received = 1;
    }
}

int main(int argc, char** argv) {
    if (argc != 4) { fprintf(stderr, "Usage: %s <levels> <max_games> <fifo>\n", argv[0]); return 1; }
    
    max_games = atoi(argv[2]);
    if (max_games <= 0) return 1;

    strncpy(levels_dir, argv[1], 255); 
    strncpy(registry_pipe, argv[3], MAX_PIPE_PATH_LENGTH-1);
    
    if (pipe(shutdown_pipe) == -1) return 1;

    open_debug_file("server_debug.log");
    debug("Starting server. Max games: %d\n", max_games);

    sessions = calloc(max_games, sizeof(session_t));
    if (!sessions) { fprintf(stderr, "Failed to allocate sessions\n"); return 1; }

    for(int i=0; i<max_games; i++) { 
        sessions[i].req_fd = sessions[i].notif_fd = -1; 
        pthread_mutex_init(&sessions[i].session_lock, NULL); 
    }
    
    init_connection_buffer(&conn_buffer);
    
    unlink(registry_pipe); 
    if (mkfifo(registry_pipe, 0666) == -1) { perror("mkfifo"); return 1; }

    struct sigaction sa_int, sa_usr1;
    
    sa_int.sa_handler = signal_handler;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0; 
    sigaction(SIGINT, &sa_int, NULL);
    
    sa_usr1.sa_handler = signal_handler;
    sigemptyset(&sa_usr1.sa_mask);
    sa_usr1.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa_usr1, NULL);
    
    signal(SIGPIPE, SIG_IGN); 

    pthread_t host_tid, *mgr_tids = malloc(max_games * sizeof(pthread_t));
    if (!mgr_tids) { free(sessions); return 1; }
    
    if (pthread_create(&host_tid, NULL, host_thread, NULL) != 0) {
        free(mgr_tids); free(sessions); return 1;
    }
    
    for(int i=0; i<max_games; i++) {
        int *id = malloc(sizeof(int));
        if (id) {
            *id = i;
            if (pthread_create(&mgr_tids[i], NULL, manager_thread, id) != 0) free(id);
        }
    }

    debug("Server running...\n");
    
    while(server_running) sleep(1); 

    debug("Shutdown signal received.\n");

    // 1. Acordar manager_threads (estão no sem_wait)
    destroy_connection_buffer(&conn_buffer);
    
    // 2. Acordar host_thread (está no select ou read do registry)
    int dummy = open(registry_pipe, O_WRONLY | O_NONBLOCK);
    if(dummy != -1) close(dummy);

    // 3. (CORREÇÃO) Remover o loop que fechava FDs aqui. 
    // As session_handlers vão acordar pelo timeout do select(), 
    // ver que server_running é 0 e sair sozinhas.

    pthread_join(host_tid, NULL); 
    for(int i=0; i<max_games; i++) pthread_join(mgr_tids[i], NULL);
    free(mgr_tids);

    cleanup_connection_resources(&conn_buffer);

    // Limpeza final de memória
    for(int i=0; i<max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        // Garante que libertamos recursos se alguma sessão ficou a meio
        if(sessions[i].active) free_session_resources(&sessions[i]);
        pthread_mutex_unlock(&sessions[i].session_lock);
        pthread_mutex_destroy(&sessions[i].session_lock);
    }
    
    pthread_mutex_destroy(&top5_mutex);
    free(sessions);
    close(shutdown_pipe[0]); close(shutdown_pipe[1]); 
    unlink(registry_pipe);
    close_debug_file();
    
    return 0;
}