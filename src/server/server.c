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
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

// ===================
// 1. GLOBAIS E ESTADO

session_t *sessions = NULL; // Active sessions
int max_games = 0;
connection_buffer_t conn_buffer; // Para gerir pedidos de comunicação

// FIX TSan Data Race: volatile não é suficiente para threads.
// Usaremos __atomic functions para aceder a esta variável.
volatile sig_atomic_t server_running = 1; 

volatile sig_atomic_t sigusr1_received = 0;
char registry_pipe[MAX_PIPE_PATH_LENGTH];
char levels_dir[256];
int shutdown_pipe[2]; 

int total_con = 0;

// Forward declarations
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

void destroy_connection_buffer(connection_buffer_t *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->active = 0;
    pthread_mutex_unlock(&buffer->mutex);
    if (buffer->empty != SEM_FAILED) sem_post(buffer->empty);
    if (buffer->full != SEM_FAILED) sem_post(buffer->full);
}

void cleanup_connection_resources(connection_buffer_t *buffer) {
    if (buffer->empty != SEM_FAILED) { sem_close(buffer->empty); sem_unlink("/pacmanist_empty"); }
    if (buffer->full != SEM_FAILED) { sem_close(buffer->full); sem_unlink("/pacmanist_full"); }
    pthread_mutex_destroy(&buffer->mutex);
}

void buffer_insert(connection_buffer_t *buffer, connection_request_t *request) {
    pthread_mutex_lock(&buffer->mutex);
    if (!buffer->active) { pthread_mutex_unlock(&buffer->mutex); return; }
    pthread_mutex_unlock(&buffer->mutex);

    if (sem_wait(buffer->empty) != 0) return;

    pthread_mutex_lock(&buffer->mutex);
    if (buffer->active) {
        buffer->requests[buffer->in] = *request;
        buffer->in = (buffer->in + 1) % BUFFER_SIZE;
        sem_post(buffer->full);
    } else {
        sem_post(buffer->empty);
    }
    pthread_mutex_unlock(&buffer->mutex);
}

int buffer_remove(connection_buffer_t *buffer, connection_request_t *request) {
    if (sem_wait(buffer->full) != 0) return -1;

    pthread_mutex_lock(&buffer->mutex);
    if (buffer->active) {
        *request = buffer->requests[buffer->out];
        buffer->out = (buffer->out + 1) % BUFFER_SIZE;
        sem_post(buffer->empty);
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }
    
    sem_post(buffer->full);
    pthread_mutex_unlock(&buffer->mutex);
    return -1;
}

// =========================================
// 3. NÍVEIS, RECURSOS E PONTUAÇÃO (Helpers)

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
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".lvl") == 0) {
            strncpy(level_files[num_levels], entry->d_name, 255);
            level_files[num_levels][255] = '\0';
            num_levels++;
        }
    }
    closedir(level_dir);

    // Nota: Poderia ordenar os níveis aqui para garantir a ordem (01.lvl, 02.lvl...)

    if (sess->current_level >= num_levels) return -1; // Sem mais níveis

    int accumulated_points = 0;
    if (sess->board && sess->board->n_pacmans > 0) accumulated_points = sess->board->pacmans[0].points;

    // FIX TSan: Garantimos que quem chama esta função já parou a thread update_sender,
    // logo é seguro fazer unload sem data race.
    if (sess->board) unload_level(sess->board);
    else {
        sess->board = malloc(sizeof(board_t));
        if (!sess->board) return -1;
    }

    debug("Session %d: Loading %s\n", sess->session_id, level_files[sess->current_level]);
    if (load_level(sess->board, level_files[sess->current_level], levels_dir, 0) != 0) return -1;

    if (sess->board->n_pacmans > 0) sess->board->pacmans[0].points = accumulated_points;
    return 0;
}

struct score_entry { int id; int pts; };

int compare_scores(const void *a, const void *b) {
    return ((struct score_entry*)b)->pts - ((struct score_entry*)a)->pts;
}

void generate_top5_file() {
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

    if (num > 1) {
        qsort(scores, num, sizeof(struct score_entry), compare_scores);
    }

    FILE *f = fopen("top5.txt", "w");
    if (f) {
        fprintf(f, "Top 5 Clientes por Pontuação\n================================\n\n");
        int limit = (num < 5) ? num : 5;
        for (int i = 0; i < limit; i++) fprintf(f, "%d. Cliente ID %d: %d pontos\n", i + 1, scores[i].id, scores[i].pts);
        if (num == 0) fprintf(f, "Nenhum cliente ativo.\n");
        fclose(f);
    }
}

// ==========================
// LÓGICA DO JOGO E PROTOCOLO

void send_board_update(session_t *sess) {
    if (!sess->board || sess->notif_fd == -1) return;
    board_t *b = sess->board;
    
    // FIX TSan: Removido 'static' para evitar buffer partilhado (Data Race)
    char msg[16384]; 
    int off = 0;

    // FIX TSan: Usar Lock de Leitura (rdlock) durante a leitura do estado do board
    pthread_rwlock_rdlock(&b->state_lock);

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
    
    pthread_rwlock_unlock(&b->state_lock);

    if (write(sess->notif_fd, msg, off) == -1) {}
}

int handle_move_result(session_t *sess, int result) {
    if (result == REACHED_PORTAL) {
        debug("Session %d: Pacman reached portal!\n", sess->session_id);
        return 2; // Indica fim de nível (caller trata do resto)
    } 
    
    if (result == DEAD_PACMAN) {
        debug("Session %d: Pacman died!\n", sess->session_id);
        send_board_update(sess); // Atualiza para mostrar morte
        return 0; // Game Over
    }
    
    return 1; // Jogo continua
}

// ============================
// THREADS (Lógica de Execução)

void* update_sender(void* arg) {
    session_t *sess = (session_t*)arg;
    while (1) {
        // Verificar se deve continuar
        pthread_mutex_lock(&sess->session_lock);
        if (!sess->game_active || !sess->board) { 
            pthread_mutex_unlock(&sess->session_lock); 
            break; 
        }
        int tempo = sess->board->tempo;
        pthread_mutex_unlock(&sess->session_lock);

        sleep_ms(tempo);

        pthread_mutex_lock(&sess->session_lock);
        // Dupla verificação após sleep
        if (sess->game_active && sess->board) {
            board_t *b = sess->board;
            pthread_rwlock_wrlock(&b->state_lock);
            
            // Movimento Monstros
            for (int i = 0; i < b->n_ghosts; i++) {
                if (b->ghosts[i].n_moves > 0)
                    move_ghost(b, i, &b->ghosts[i].moves[b->ghosts[i].current_move % b->ghosts[i].n_moves]);
            }
            
            pthread_rwlock_unlock(&b->state_lock);
            
            send_board_update(sess); // O rdlock é feito lá dentro
        }
        pthread_mutex_unlock(&sess->session_lock);
    }
    return NULL;
}

void* session_handler(void* arg) {
    session_t *sess = (session_t*)arg;
    debug("Session %d handler started\n", sess->session_id);
    
    if (sess->req_fd == -1 || sess->notif_fd == -1) {
        sess->active = 0; 
        return NULL;
    }

    pthread_mutex_lock(&sess->session_lock);
    send_board_update(sess);
    pthread_mutex_unlock(&sess->session_lock);
    
    if (pthread_create(&sess->update_thread, NULL, update_sender, sess) != 0) {
        sess->game_active = 0;
        close(sess->req_fd);
        close(sess->notif_fd);
        sess->active = 0;
        return NULL;
    }

    char buf[256];
    while (sess->game_active) {
        if (read(sess->req_fd, buf, sizeof(buf)) <= 0) break;

        if (buf[0] == OP_CODE_DISCONNECT) {
             pthread_mutex_lock(&sess->session_lock);
             sess->game_active = 0;
             char resp[] = { OP_CODE_DISCONNECT, 0 };
             if (write(sess->notif_fd, resp, 2) == -1) {}
             pthread_mutex_unlock(&sess->session_lock);
             break;
        }

        pthread_mutex_lock(&sess->session_lock);
        if (!sess->game_active) {
            pthread_mutex_unlock(&sess->session_lock);
            break;
        }

        if (buf[0] == OP_CODE_PLAY && sess->board && sess->board->n_pacmans > 0) {
            command_t cmd = { .command = buf[1], .turns = 1, .turns_left = 1 };
            board_t *current_board = sess->board;
            
            // Proteger lógica do movimento
            pthread_rwlock_wrlock(&current_board->state_lock);
            int res = move_pacman(current_board, 0, &cmd);
            pthread_rwlock_unlock(&current_board->state_lock);
            
            int move_result = handle_move_result(sess, res);
            pthread_mutex_unlock(&sess->session_lock);
            
            // --- Lógica de Transição de Nível ---
            if (move_result == 2) { // REACHED_PORTAL
                
                // 1. Sinalizar paragem do jogo (threads auxiliares leem isto)
                pthread_mutex_lock(&sess->session_lock);
                sess->game_active = 0;
                pthread_mutex_unlock(&sess->session_lock);
                
                // 2. JOIN OBRIGATÓRIO: Esperar que update_sender termine
                // Isto evita que update_sender use o board enquanto fazemos unload
                pthread_join(sess->update_thread, NULL);
                
                // 3. Agora é seguro carregar o próximo nível
                pthread_mutex_lock(&sess->session_lock);
                sess->current_level++;
                if (load_next_level(sess) == 0) {
                    sess->victory = 1; 
                    send_board_update(sess); // Envia board novo com flag de vitória
                    sess->victory = 0;
                    sess->game_active = 1; // Reativa jogo
                    
                    // Reiniciar thread de monstros
                    if (pthread_create(&sess->update_thread, NULL, update_sender, sess) != 0) {
                        sess->game_active = 0;
                    }
                } else {
                    // Sem mais níveis ou erro
                    sess->victory = 1;
                    send_board_update(sess); // Board final (ou vazio)
                    sess->game_active = 0;
                }
                pthread_mutex_unlock(&sess->session_lock);
            } 
            // --- Fim Transição ---
            
            else if (move_result == 1) { // Jogo normal
                pthread_mutex_lock(&sess->session_lock);
                send_board_update(sess);
                pthread_mutex_unlock(&sess->session_lock);
            }
            // se move_result == 0 (Game Over), loop continua mas game_active pode ser 0
        } else {
            pthread_mutex_unlock(&sess->session_lock);
        }
    }

    // Cleanup final da sessão
    pthread_mutex_lock(&sess->session_lock);
    sess->game_active = 0;
    pthread_mutex_unlock(&sess->session_lock);
    
    // Se a thread ainda estiver a correr (ex: disconnect abrupto), esperar por ela
    // Nota: Em alguns casos o join pode falhar se a thread já terminou, mas é seguro tentar.
    pthread_join(sess->update_thread, NULL); 
    
    free_session_resources(sess);
    
    pthread_mutex_lock(&sess->session_lock);
    sess->active = 0;
    pthread_mutex_unlock(&sess->session_lock);
    
    debug("Session %d ended (Slot freed)\n", sess->session_id);
    return NULL;
}

void* manager_thread(void* arg) {
    int id = *(int*)arg; free(arg);
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1); pthread_sigmask(SIG_BLOCK, &mask, NULL);
    debug("Manager %d started\n", id);

    // FIX TSan: Leitura atómica
    while (__atomic_load_n(&server_running, __ATOMIC_RELAXED)) {
        connection_request_t req;
        if (buffer_remove(&conn_buffer, &req) != 0) {
            if (!__atomic_load_n(&server_running, __ATOMIC_RELAXED)) break;
            continue;
        }

        int sess_id = -1;
        for (int i = 0; i < max_games; i++) {
            pthread_mutex_lock(&sessions[i].session_lock);
            if (!sessions[i].active) {
                sess_id = i; 
                sessions[i].active = 1; 
                
                char *filename = strrchr(req.req_pipe_path, '/');
                if (filename) filename++; else filename = req.req_pipe_path;
                
                sessions[i].session_id = atoi(filename);
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
            buffer_insert(&conn_buffer, &req);
            sleep(1);
            continue;
        }
        
        session_t *sess = &sessions[sess_id];
        
        sess->req_fd = open(req.req_pipe_path, O_RDONLY);
        if (sess->req_fd == -1) {
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        sess->notif_fd = open(req.notif_pipe_path, O_WRONLY);
        if (sess->notif_fd == -1) {
            close(sess->req_fd);
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        char confirm_msg[2] = { OP_CODE_CONNECT, 0 };
        if (write(sess->notif_fd, confirm_msg, 2) == -1) {
            close(sess->req_fd);
            close(sess->notif_fd);
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        sess->board = NULL;
        
        if (load_next_level(sess) != 0) {
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

void* host_thread(void* arg) {
    (void)arg;
    debug("Host thread started\n");
    
    int reg_fd = open(registry_pipe, O_RDWR | O_NONBLOCK);
    if (reg_fd == -1) { debug("Failed to open registry\n"); return NULL; }
    
    fcntl(reg_fd, F_SETFL, fcntl(reg_fd, F_GETFL) & ~O_NONBLOCK);

    // FIX TSan: Leitura atómica
    while (__atomic_load_n(&server_running, __ATOMIC_RELAXED)) {
        if (sigusr1_received) { sigusr1_received = 0; generate_top5_file(); }
        
        char buf[1 + MAX_PIPE_PATH_LENGTH * 3];
        ssize_t n = read(reg_fd, buf, sizeof(buf));
        
        if (!__atomic_load_n(&server_running, __ATOMIC_RELAXED)) break;

        if (n <= 0) {
            if (n == 0) { 
                close(reg_fd);
                reg_fd = open(registry_pipe, O_RDWR | O_NONBLOCK);
                if (reg_fd != -1) fcntl(reg_fd, F_SETFL, fcntl(reg_fd, F_GETFL) & ~O_NONBLOCK);
                else sleep_ms(100);
            } else if (errno != EINTR) {
                sleep_ms(100);
            }
            continue;
        }

        if (buf[0] == OP_CODE_CONNECT) {
            total_con += 1;
            connection_request_t req;
            memcpy(req.req_pipe_path, buf + 1, MAX_PIPE_PATH_LENGTH);
            memcpy(req.notif_pipe_path, buf + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
            req.req_pipe_path[MAX_PIPE_PATH_LENGTH-1] = req.notif_pipe_path[MAX_PIPE_PATH_LENGTH-1] = '\0';
            
            buffer_insert(&conn_buffer, &req);
        }
    }
    close(reg_fd);
    debug("Host thread ended\n");
    return NULL;
}

// ==================================================
// MAIN E GESTÃO DE SINAIS

void signal_handler(int signum) {
    if (signum == SIGINT) {
        // FIX TSan: Escrita atómica no handler
        __atomic_store_n(&server_running, 0, __ATOMIC_RELAXED);
        
        char c = 1; 
        if (write(shutdown_pipe[1], &c, 1) == -1) {} 
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

    signal(SIGINT, signal_handler); 
    signal(SIGUSR1, signal_handler); 
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
    
    // FIX TSan: Leitura atómica no loop principal
    while(__atomic_load_n(&server_running, __ATOMIC_RELAXED)) sleep(1);

    debug("Shutdown signal received.\n");

    destroy_connection_buffer(&conn_buffer);
    
    // Desbloquear a host_thread
    int dummy = open(registry_pipe, O_WRONLY | O_NONBLOCK);
    if(dummy != -1) close(dummy);

    pthread_join(host_tid, NULL);
    for(int i=0; i<max_games; i++) pthread_join(mgr_tids[i], NULL);
    free(mgr_tids);

    cleanup_connection_resources(&conn_buffer);

    for(int i=0; i<max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        if(sessions[i].active) free_session_resources(&sessions[i]);
        pthread_mutex_unlock(&sessions[i].session_lock);
        pthread_mutex_destroy(&sessions[i].session_lock);
    }
    
    free(sessions);
    close(shutdown_pipe[0]); close(shutdown_pipe[1]); 
    unlink(registry_pipe);
    close_debug_file();
    
    return 0;
}