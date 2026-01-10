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

session_t *sessions = NULL; 
int max_games = 0;
connection_buffer_t conn_buffer; 
_Atomic int server_running = 1;
_Atomic int sigusr1_received = 0;
char registry_pipe[MAX_PIPE_PATH_LENGTH];
char levels_dir[256];
int shutdown_pipe[2]; 

// Evita acesso ao diretório (opendir/readdir) durante o jogo crítico
char cached_level_files[100][256];
int cached_num_levels = 0;

void init_level_cache(const char *dir_path) {
    DIR* level_dir = opendir(dir_path);
    if (!level_dir) {
        perror("Failed to cache levels");
        exit(1);
    }

    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL && cached_num_levels < 100) {
        if (entry->d_name[0] == '.') continue;
        size_t len = strlen(entry->d_name);
        // Filtra apenas ficheiros .lvl
        if (len > 4 && strcmp(entry->d_name + len - 4, ".lvl") == 0) {
            strncpy(cached_level_files[cached_num_levels], entry->d_name, 255);
            cached_level_files[cached_num_levels][255] = '\0';
            cached_num_levels++;
        }
    }
    closedir(level_dir);
}

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
    // Acesso direto à memória pré-carregada (muito mais rápido que opendir)
    if (sess->current_level >= cached_num_levels) {
        debug("Session %d: No more levels (current: %d)\n", sess->session_id, sess->current_level);
        return -1; 
    }

    int accumulated_points = 0;
    if (sess->board && sess->board->n_pacmans > 0) accumulated_points = sess->board->pacmans[0].points;

    if (sess->board) unload_level(sess->board);
    else {
        sess->board = malloc(sizeof(board_t));
        if (!sess->board) {
            debug("Session %d: Failed to allocate board memory\n", sess->session_id);
            return -1;
        }
    }

    debug("Session %d: Loading %s\n", sess->session_id, cached_level_files[sess->current_level]);
    
    if (load_level(sess->board, cached_level_files[sess->current_level], levels_dir, 0) != 0) return -1;

    if (sess->board->n_pacmans > 0) sess->board->pacmans[0].points = accumulated_points;
    return 0;
}

struct score_entry { int id; int pts; };

int compare_scores(const void *a, const void *b) {
    return ((struct score_entry*)b)->pts - ((struct score_entry*)a)->pts;
}

void generate_top5_file() {
    debug("Generating top 5 clients file...\n");
    
    // Alocação dinâmica para evitar stack overflow com muitos jogos
    struct score_entry *scores = malloc(max_games * sizeof(struct score_entry));
    if (!scores) {
        debug("Failed to allocate memory for scores\n");
        return; 
    }

    int num = 0;

    // Snapshot rápido dos dados 
    for (int i = 0; i < max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        if (sessions[i].active && sessions[i].board) {
            scores[num].id = sessions[i].session_id;
            scores[num].pts = (sessions[i].board->n_pacmans > 0) ? sessions[i].board->pacmans[0].points : 0;
            num++;
        }
        pthread_mutex_unlock(&sessions[i].session_lock);
    }
    
    // Quicksort das pontuações
    if (num > 1) {
        qsort(scores, num, sizeof(struct score_entry), compare_scores);
    }

    FILE *f = fopen("top5.txt", "w");
    if (f) {
        fprintf(f, "Top 5 Clientes por Pontuação\n================================\n\n");
        int limit = (num < 5) ? num : 5;
        for (int i = 0; i < limit; i++) {
            fprintf(f, "%d. Cliente ID %d: %d pontos\n", i + 1, scores[i].id, scores[i].pts);
        }
        if (num == 0) fprintf(f, "Nenhum cliente ativo.\n");
        fclose(f);
        debug("Top 5 generated with %d clients\n", limit);
    } else {
        debug("Failed to open top5.txt for writing\n");
    }

    free(scores);
}

// ==========================
// LÓGICA DO JOGO E PROTOCOLO

// Envia o board update para o server
void send_board_update(session_t *sess) {
    if (!sess->board || sess->notif_fd == -1) return;
    board_t *b = sess->board;
    
    // Buffer grande para evitar overflows em mapas grandes
    char msg[16384]; 
    int off = 0;

    // Header fixo
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
        char content = b->board[i].content;
        char out_char;

        switch(content) {
            case 'W': out_char = '#'; break;
            case 'P': out_char = 'C'; break;
            case 'M': out_char = 'M'; break;
            default:
                if (b->board[i].has_dot) out_char = '.';
                else if (b->board[i].has_portal) out_char = '@';
                else out_char = ' ';
                break;
        }
        msg[off++] = out_char;
    }
    
    if (write(sess->notif_fd, msg, off) == -1) {} // Ignorar erro de pipe
}

int handle_move_result(session_t *sess, int result) {
    if (result == REACHED_PORTAL) {
        debug("Session %d: Pacman reached portal!\n", sess->session_id);
        
        pthread_mutex_lock(&sess->session_lock);
        sess->current_level++;
        
        // Carrega proximo nível
        if (load_next_level(sess) != 0) {
            sess->victory = 1;
            sess->game_active = 0;
            send_board_update(sess);
            pthread_mutex_unlock(&sess->session_lock);
            return 0; 
        }
        
        sess->game_active = 1;
        send_board_update(sess);
        pthread_mutex_unlock(&sess->session_lock);
        
        if (pthread_create(&sess->update_thread, NULL, update_sender, sess) != 0) {
            debug("Session %d: Failed to restart update thread\n", sess->session_id);
            pthread_mutex_lock(&sess->session_lock);
            sess->game_active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            return 0;
        }
        return 2; 
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
    
    // O jogo termina quando se deixarem de criar updates de threads
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
    
    // Pipe passa a modo nao bloqueante, permitindo que o servidor
    // leia dados sem ficar à espera
    int flags = fcntl(sess->req_fd, F_GETFL, 0);
    fcntl(sess->req_fd, F_SETFL, flags | O_NONBLOCK);

    while (keep_running && server_running) { 
        pthread_mutex_lock(&sess->session_lock);
        keep_running = sess->game_active;
        pthread_mutex_unlock(&sess->session_lock);
        
        if (!keep_running) break;
        
        // Tentar ler sem bloquear
        ssize_t bytes_read = read(sess->req_fd, buf, sizeof(buf));
        
        if (bytes_read > 0) {
            // Dados recebidos - processar
        } else if (bytes_read == 0) {
            debug("Client disconnected or error\n");
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sleep_ms(50);
                continue;
            } else if (errno == EINTR) {
                continue;
            } else {
                debug("Session %d: Read error: %s\n", sess->session_id, strerror(errno));
                break;
            }
        }

        // Processamento de comandos
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
                
                // Confirma se passa de nível ou se acaba o jogo
                int move_result = handle_move_result(sess, res);
                if (move_result == 2) {
                    update_thread_joined = 0; 
                } else if (move_result == 0) {
                    break;
                }
                continue;
            }
            
            // Confirma se o pacman morreu ou de o jogo continua
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

        char *filename = strrchr(req.req_pipe_path, '/');
        if (filename) filename++; else filename = req.req_pipe_path;
        int requested_id = atoi(filename);

        int sess_id = -1;
        for (int i = 0; i < max_games; i++) {
            pthread_mutex_lock(&sessions[i].session_lock);
            if (!sessions[i].active) {
                sess_id = i; 
                sessions[i].active = 1; 
                
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
            // Passa a ser gerido pelos semáforos
            buffer_insert(&conn_buffer, &req);
            sleep(1); 
            continue;
        }
        
        session_t *sess = &sessions[sess_id];
        
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

void* host_thread(void* arg) {
    (void)arg;
    debug("Host thread started\n");
    
    int reg_fd = open(registry_pipe, O_RDWR | O_NONBLOCK);
    if (reg_fd == -1) { debug("Failed to open registry\n"); return NULL; }

    while (server_running) {
        if (sigusr1_received) { 
            sigusr1_received = 0; 
            generate_top5_file(); 
        }
        
        char buf[1 + MAX_PIPE_PATH_LENGTH * 3];
        ssize_t n = read(reg_fd, buf, sizeof(buf));
        
        if (n <= 0) {
            if (n == 0) { 
                // EOF - reabrir o pipe
                close(reg_fd);
                reg_fd = open(registry_pipe, O_RDWR | O_NONBLOCK);
                if (reg_fd == -1) sleep_ms(100);
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                sleep_ms(100);
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
    
    // Inicialização da cache de níveis
    init_level_cache(levels_dir);

    if (pipe(shutdown_pipe) == -1) return 1;

    open_debug_file("server_debug.log");
    debug("Starting server. Max games: %d. Levels cached: %d\n", max_games, cached_num_levels);

    sessions = calloc(max_games, sizeof(session_t));
    if (!sessions) { fprintf(stderr, "Failed to allocate sessions\n"); return 1; }

    // Colocar um mutex em cada sessão
    for(int i=0; i<max_games; i++) { 
        sessions[i].req_fd = sessions[i].notif_fd = -1; 
        pthread_mutex_init(&sessions[i].session_lock, NULL); 
    }
    
    init_connection_buffer(&conn_buffer);
    
    unlink(registry_pipe); 
    if (mkfifo(registry_pipe, 0666) == -1) { perror("mkfifo"); return 1; }

    struct sigaction sa_int, sa_usr1;
    
    // Aponta para a funcao signal_handler
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

    destroy_connection_buffer(&conn_buffer);
    
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