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

// --- Globais ---
session_t *sessions = NULL;
int max_games = 0;
connection_buffer_t conn_buffer;
volatile sig_atomic_t server_running = 1;
volatile sig_atomic_t sigusr1_received = 0;
char registry_pipe[MAX_PIPE_PATH_LENGTH];
char levels_dir[256];
int shutdown_pipe[2]; // Mantido para compatibilidade, usado no signal handler

// --- Macros e Helpers ---
// Simplifica a serialização de inteiros para o buffer de rede
#define PACK_INT(msg, offset, val) do { int v = (val); memcpy((msg) + (offset), &v, sizeof(int)); (offset) += sizeof(int); } while(0)

// Declaração antecipada
void send_board_update(session_t *sess);
int load_next_level(session_t *sess);

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

// Centraliza a lógica de "O que acontece depois de um movimento?"
// Retorna 1 se o jogo continua, 0 se o jogo deve parar (Game Over ou Vitória Final)
int handle_move_result(session_t *sess, int result) {
    if (result == REACHED_PORTAL) {
        debug("Session %d: Pacman reached portal!\n", sess->session_id);
        sess->current_level++;
        
        // Tenta carregar o próximo nível
        if (load_next_level(sess) != 0) {
            // Se falhar (não há mais níveis), é vitória total
            sess->victory = 1;
            send_board_update(sess);
            sess->game_active = 0;
            return 0; 
        }
        // Sucesso ao carregar novo nível
        send_board_update(sess);
        return 1;
    } 
    
    if (result == DEAD_PACMAN) {
        debug("Session %d: Pacman died!\n", sess->session_id);
        send_board_update(sess); // Atualiza para mostrar morte
        sess->game_active = 0;
        return 0;
    }
    
    return 1; // Nada especial aconteceu, jogo continua
}

// --- Gestão do Buffer Produtor-Consumidor ---

void init_connection_buffer(connection_buffer_t *buffer) {
    buffer->in = 0; buffer->out = 0; buffer->active = 1;
    // Semáforos nomeados para macOS/Linux
    sem_unlink("/pacmanist_empty"); sem_unlink("/pacmanist_full");
    buffer->empty = sem_open("/pacmanist_empty", O_CREAT | O_EXCL, 0644, BUFFER_SIZE);
    buffer->full = sem_open("/pacmanist_full", O_CREAT | O_EXCL, 0644, 0);
    
    if (buffer->empty == SEM_FAILED || buffer->full == SEM_FAILED) { 
        perror("sem_open failed"); exit(1); 
    }
    pthread_mutex_init(&buffer->mutex, NULL);
}

// Apenas sinaliza paragem (não destrói mutexes ainda)
void destroy_connection_buffer(connection_buffer_t *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->active = 0;
    pthread_mutex_unlock(&buffer->mutex);
    // Acorda threads bloqueadas
    if (buffer->empty != SEM_FAILED) sem_post(buffer->empty);
    if (buffer->full != SEM_FAILED) sem_post(buffer->full);
}

// Destrói recursos (chamar apenas no fim do main)
void cleanup_connection_resources(connection_buffer_t *buffer) {
    if (buffer->empty != SEM_FAILED) { sem_close(buffer->empty); sem_unlink("/pacmanist_empty"); }
    if (buffer->full != SEM_FAILED) { sem_close(buffer->full); sem_unlink("/pacmanist_full"); }
    pthread_mutex_destroy(&buffer->mutex);
}

void buffer_insert(connection_buffer_t *buffer, connection_request_t *request) {
    pthread_mutex_lock(&buffer->mutex);
    if (!buffer->active) { pthread_mutex_unlock(&buffer->mutex); return; }
    pthread_mutex_unlock(&buffer->mutex);

    sem_wait(buffer->empty);

    pthread_mutex_lock(&buffer->mutex);
    if (buffer->active) {
        buffer->requests[buffer->in] = *request;
        buffer->in = (buffer->in + 1) % BUFFER_SIZE;
        sem_post(buffer->full);
    } else {
        sem_post(buffer->empty); // Devolve o token se estiver inativo
    }
    pthread_mutex_unlock(&buffer->mutex);
}

int buffer_remove(connection_buffer_t *buffer, connection_request_t *request) {
    pthread_mutex_lock(&buffer->mutex);
    if (!buffer->active) { pthread_mutex_unlock(&buffer->mutex); return -1; }
    pthread_mutex_unlock(&buffer->mutex);

    // Tentativa com timeout manual (compatível com macOS)
    int attempts = 0;
    while (attempts++ < 10) {
        pthread_mutex_lock(&buffer->mutex);
        if (!buffer->active) { pthread_mutex_unlock(&buffer->mutex); return -1; }
        pthread_mutex_unlock(&buffer->mutex);

        if (sem_trywait(buffer->full) == 0) break;
        if (errno != EAGAIN) return -1;
        sleep_ms(100);
    }
    if (attempts >= 10) return -1;

    pthread_mutex_lock(&buffer->mutex);
    if (buffer->active) {
        *request = buffer->requests[buffer->out];
        buffer->out = (buffer->out + 1) % BUFFER_SIZE;
        sem_post(buffer->empty);
        pthread_mutex_unlock(&buffer->mutex);
        return 0;
    }
    sem_post(buffer->full); // Devolve token
    pthread_mutex_unlock(&buffer->mutex);
    return -1;
}

// --- Lógica de Níveis e Ficheiros ---

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
        char *dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".lvl") == 0) {
            // CORREÇÃO: Usar snprintf em vez de strncpy para evitar erro de truncation
            snprintf(level_files[num_levels++], 256, "%s", entry->d_name);
        }
    }
    closedir(level_dir);

    // Bubble sort para ordenar níveis
    for (int i = 0; i < num_levels - 1; i++) {
        for (int j = i + 1; j < num_levels; j++) {
            if (strcmp(level_files[i], level_files[j]) > 0) {
                char temp[256];
                strcpy(temp, level_files[i]); strcpy(level_files[i], level_files[j]); strcpy(level_files[j], temp);
            }
        }
    }

    debug("Session %d: Found %d levels, current: %d\n", sess->session_id, num_levels, sess->current_level);

    if (sess->current_level >= num_levels) return -1; // Sem mais níveis

    // Preservar pontos
    int accumulated_points = 0;
    if (sess->board && sess->board->n_pacmans > 0) accumulated_points = sess->board->pacmans[0].points;

    // Recarregar board
    if (sess->board) unload_level(sess->board);
    else sess->board = malloc(sizeof(board_t));

    debug("Session %d: Loading %s\n", sess->session_id, level_files[sess->current_level]);
    if (load_level(sess->board, level_files[sess->current_level], levels_dir, 0) != 0) return -1;

    // Restaurar pontos
    if (sess->board->n_pacmans > 0) sess->board->pacmans[0].points = accumulated_points;
    return 0;
}

void generate_top5_file() {
    debug("Generating top 5 clients file...\n");
    struct { int id; int pts; } scores[MAX_SESSIONS], temp;
    int num = 0;

    // Coletar
    for (int i = 0; i < max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        if (sessions[i].active && sessions[i].board) {
            scores[num].id = sessions[i].session_id;
            scores[num].pts = (sessions[i].board->n_pacmans > 0) ? sessions[i].board->pacmans[0].points : 0;
            num++;
        }
        pthread_mutex_unlock(&sessions[i].session_lock);
    }

    // Ordenar
    for (int i = 0; i < num - 1; i++) 
        for (int j = i + 1; j < num; j++) 
            if (scores[j].pts > scores[i].pts) { temp = scores[i]; scores[i] = scores[j]; scores[j] = temp; }

    // Escrever
    FILE *f = fopen("top5.txt", "w");
    if (f) {
        fprintf(f, "Top 5 Clientes por Pontuação\n================================\n\n");
        int limit = (num < 5) ? num : 5;
        for (int i = 0; i < limit; i++) fprintf(f, "%d. Cliente ID %d: %d pontos\n", i + 1, scores[i].id, scores[i].pts);
        if (num == 0) fprintf(f, "Nenhum cliente ativo.\n");
        fclose(f);
        debug("Top 5 generated with %d clients\n", limit);
    }
}

void signal_handler(int signum) {
    if (signum == SIGINT) {
        server_running = 0;
        char c = 1; 
        // CORREÇÃO: Verificar retorno do write (num handler não devemos fazer print complexo em erro)
        if (write(shutdown_pipe[1], &c, 1) == -1) {
            // Ignorar erro silenciosamente dentro do signal handler
        }
    } else if (signum == SIGUSR1) {
        sigusr1_received = 1;
        // debug usa printf, que não é async-signal-safe, mas para debug simples é "aceitável"
        // O ideal seria apenas setar a flag.
        // debug("SIGUSR1 received\n"); 
    }
}

void send_board_update(session_t *sess) {
    if (!sess->board || sess->notif_fd == -1) return;
    board_t *b = sess->board;
    char msg[8192]; 
    int off = 0;

    // Serialização compacta
    msg[off++] = OP_CODE_BOARD;
    PACK_INT(msg, off, b->width);
    PACK_INT(msg, off, b->height);
    PACK_INT(msg, off, b->tempo);
    PACK_INT(msg, off, sess->victory);
    PACK_INT(msg, off, (b->n_pacmans > 0 && !b->pacmans[0].alive) ? 1 : 0); // Game Over
    PACK_INT(msg, off, (b->n_pacmans > 0) ? b->pacmans[0].points : 0);

    // Board content
    for (int i = 0; i < b->width * b->height; i++) {
        char ch = b->board[i].content;
        if (ch == 'W') msg[off++] = '#';
        else if (ch == 'P') msg[off++] = 'C';
        else if (ch == 'M') msg[off++] = 'M';
        else msg[off++] = (b->board[i].has_portal) ? '@' : ((b->board[i].has_dot) ? '.' : ' ');
    }
    
    // CORREÇÃO: Verificar retorno do write
    if (write(sess->notif_fd, msg, off) == -1) {
        // Se falhar a escrita, o cliente provavelmente desconectou-se
        // debug("Failed to send board update\n");
    }
}

// --- Threads do Servidor ---

void* update_sender(void* arg) {
    session_t *sess = (session_t*)arg;
    while (sess->game_active) {
        pthread_mutex_lock(&sess->session_lock);
        if (!sess->game_active || !sess->board) { pthread_mutex_unlock(&sess->session_lock); break; }
        int tempo = sess->board->tempo;
        pthread_mutex_unlock(&sess->session_lock);

        sleep_ms(tempo);

        pthread_mutex_lock(&sess->session_lock);
        if (sess->game_active && sess->board) {
            board_t *b = sess->board;
            pthread_rwlock_wrlock(&b->state_lock);
            
            // Movimento automático Pacman
            if (b->n_pacmans > 0 && b->pacmans[0].n_moves > 0 && b->pacmans[0].alive) {
                int res = move_pacman(b, 0, &b->pacmans[0].moves[b->pacmans[0].current_move % b->pacmans[0].n_moves]);
                
                // Unlock antes de processar resultado (pois pode demorar/carregar nível)
                pthread_rwlock_unlock(&b->state_lock); 
                
                if (!handle_move_result(sess, res)) {
                    // Jogo acabou ou mudou de nível (e falhou), sair
                    pthread_mutex_unlock(&sess->session_lock);
                    continue; 
                }
                
                // Relock para os monstros
                pthread_rwlock_wrlock(&b->state_lock); 
            }

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

void* host_thread(void* arg) {
    (void)arg;
    debug("Host thread started\n");
    
    // Abre com NONBLOCK para não travar se falhar logo, depois muda para BLOCKING
    int reg_fd = open(registry_pipe, O_RDONLY | O_NONBLOCK);
    if (reg_fd == -1) { debug("Failed to open registry\n"); return NULL; }
    
    // Remover O_NONBLOCK
    fcntl(reg_fd, F_SETFL, fcntl(reg_fd, F_GETFL) & ~O_NONBLOCK);

    while (server_running) {
        if (sigusr1_received) { sigusr1_received = 0; generate_top5_file(); }
        
        char buf[256];
        ssize_t n = read(reg_fd, buf, sizeof(buf));
        if (!server_running) break;

        if (n <= 0) {
            if (n == 0) { // EOF - reabrir
                close(reg_fd);
                reg_fd = open(registry_pipe, O_RDONLY | O_NONBLOCK);
                if (reg_fd != -1) fcntl(reg_fd, F_SETFL, fcntl(reg_fd, F_GETFL) & ~O_NONBLOCK);
                else sleep_ms(100);
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

void* session_handler(void* arg) {
    session_t *sess = (session_t*)arg;
    debug("Session %d handler started\n", sess->session_id);
    
    if ((sess->req_fd = open(sess->req_pipe_path, O_RDONLY)) == -1) { 
        sess->active = 0; return NULL; 
    }
    if ((sess->notif_fd = open(sess->notif_pipe_path, O_WRONLY)) == -1) { 
        close(sess->req_fd); sess->active = 0; return NULL; 
    }

    pthread_mutex_lock(&sess->session_lock);
    send_board_update(sess);
    pthread_mutex_unlock(&sess->session_lock);
    
    pthread_create(&sess->update_thread, NULL, update_sender, sess);

    char buf[256];
    while (sess->game_active) {
        // Se ler <= 0 (erro ou EOF), sai do loop
        if (read(sess->req_fd, buf, sizeof(buf)) <= 0) {
            debug("Client disconnected or error\n");
            break; 
        }

        pthread_mutex_lock(&sess->session_lock);
        if (buf[0] == OP_CODE_DISCONNECT) {
            sess->game_active = 0;
            char resp[] = { OP_CODE_DISCONNECT, 0 };
            
            // CORREÇÃO: Verificar retorno do write
            if (write(sess->notif_fd, resp, 2) == -1) {
                perror("Failed to confirm disconnect");
            }

        } else if (buf[0] == OP_CODE_PLAY && sess->board && sess->board->n_pacmans > 0) {
            // Processa movimento manual apenas se não houver movimentos automáticos
            if (sess->board->pacmans[0].n_moves == 0) {
                command_t cmd = { .command = buf[1], .turns = 1, .turns_left = 1 };
                
                pthread_rwlock_wrlock(&sess->board->state_lock);
                int res = move_pacman(sess->board, 0, &cmd);
                pthread_rwlock_unlock(&sess->board->state_lock);

                // Usa a função centralizada para verificar vitória/derrota
                if (handle_move_result(sess, res)) {
                    send_board_update(sess);
                }
            }
        }
        pthread_mutex_unlock(&sess->session_lock);
    }

    // --- CORREÇÃO DEADLOCK ---
    // Garante que a flag está a 0 para libertar a update_thread
    pthread_mutex_lock(&sess->session_lock);
    sess->game_active = 0;
    pthread_mutex_unlock(&sess->session_lock);
    
    pthread_join(sess->update_thread, NULL);

    // Limpa recursos (pipes, memória)
    free_session_resources(sess);
    
    // --- CORREÇÃO MAX_GAMES ---
    // Liberta explicitamente o slot da sessão
    pthread_mutex_lock(&sess->session_lock);
    sess->active = 0;
    pthread_mutex_unlock(&sess->session_lock);
    
    debug("Session %d ended (Slot freed)\n", sess->session_id);
    return NULL;
}

void* manager_thread(void* arg) {
    int id = *(int*)arg; free(arg);
    // Bloquear SIGUSR1
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1); pthread_sigmask(SIG_BLOCK, &mask, NULL);
    debug("Manager %d started\n", id);

    while (server_running) {
        connection_request_t req;
        if (buffer_remove(&conn_buffer, &req) != 0) continue;

        int sess_id = -1;
        // Procura slot livre
        for (int i = 0; i < max_games; i++) {
            pthread_mutex_lock(&sessions[i].session_lock);
            if (!sessions[i].active) {
                sess_id = i; 
                sessions[i].active = 1; // Reserva slot
                sessions[i].session_id = i;
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

        if (sess_id == -1) { debug("Manager %d: No slots\n", id); continue; }
        
        session_t *sess = &sessions[sess_id];
        sess->board = NULL;
        
        if (load_next_level(sess) != 0) {
            pthread_mutex_lock(&sess->session_lock); sess->active = 0; pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        // Executa sessão nesta thread
        session_handler(sess);
    }
    debug("Manager %d ended\n", id);
    return NULL;
}

// --- Main ---

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
    for(int i=0; i<max_games; i++) { 
        sessions[i].req_fd = sessions[i].notif_fd = -1; 
        pthread_mutex_init(&sessions[i].session_lock, NULL); 
    }
    
    init_connection_buffer(&conn_buffer);
    
    unlink(registry_pipe); 
    if (mkfifo(registry_pipe, 0666) == -1) { perror("mkfifo"); return 1; }

    // Handlers
    signal(SIGINT, signal_handler); 
    signal(SIGUSR1, signal_handler); 
    signal(SIGPIPE, SIG_IGN); // Ignora SIGPIPE para não crashar se cliente cair

    // Threads
    pthread_t host_tid, *mgr_tids = malloc(max_games * sizeof(pthread_t));
    pthread_create(&host_tid, NULL, host_thread, NULL);
    for(int i=0; i<max_games; i++) {
        int *id = malloc(sizeof(int)); *id = i; 
        pthread_create(&mgr_tids[i], NULL, manager_thread, id);
    }

    debug("Server running...\n");
    
    // Loop principal (espera sinal)
    while(server_running) sleep(1);

    debug("Shutdown signal received.\n");

    // 1. Sinaliza paragem
    destroy_connection_buffer(&conn_buffer);
    
    // 2. Acorda host_thread (envia EOF pelo pipe)
    int dummy = open(registry_pipe, O_WRONLY | O_NONBLOCK);
    if(dummy != -1) close(dummy);

    // 3. Aguarda threads
    pthread_join(host_tid, NULL);
    for(int i=0; i<max_games; i++) pthread_join(mgr_tids[i], NULL);
    free(mgr_tids);

    // 4. Limpeza final
    cleanup_connection_resources(&conn_buffer);

    for(int i=0; i<max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        if(sessions[i].active) free_session_resources(&sessions[i]); // Reutiliza limpeza
        pthread_mutex_unlock(&sessions[i].session_lock);
        pthread_mutex_destroy(&sessions[i].session_lock);
    }
    
    free(sessions);
    close(shutdown_pipe[0]); close(shutdown_pipe[1]); 
    unlink(registry_pipe);
    close_debug_file();
    
    return 0;
}