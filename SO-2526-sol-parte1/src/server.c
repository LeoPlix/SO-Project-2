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


session_t *sessions = NULL;
int max_games = 0;
connection_buffer_t conn_buffer;
int server_running = 1;
char registry_pipe[MAX_PIPE_PATH_LENGTH];
char levels_dir[256];

// Inicializar buffer produtor-consumidor
void init_connection_buffer(connection_buffer_t *buffer) {
    buffer->in = 0;
    buffer->out = 0;
    buffer->active = 1;
    
    // Usar semáforos nomeados para compatibilidade com macOS
    sem_unlink("/pacmanist_empty");
    sem_unlink("/pacmanist_full");
    
    buffer->empty = sem_open("/pacmanist_empty", O_CREAT | O_EXCL, 0644, BUFFER_SIZE);
    buffer->full = sem_open("/pacmanist_full", O_CREAT | O_EXCL, 0644, 0);
    
    if (buffer->empty == SEM_FAILED || buffer->full == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }
    
    pthread_mutex_init(&buffer->mutex, NULL);
}

// Destruir buffer produtor-consumidor
void destroy_connection_buffer(connection_buffer_t *buffer) {
    buffer->active = 0;
    
    if (buffer->empty != SEM_FAILED) {
        sem_close(buffer->empty);
        sem_unlink("/pacmanist_empty");
    }
    if (buffer->full != SEM_FAILED) {
        sem_close(buffer->full);
        sem_unlink("/pacmanist_full");
    }
    pthread_mutex_destroy(&buffer->mutex);
}

// Inserir pedido no buffer (produtor)
void buffer_insert(connection_buffer_t *buffer, connection_request_t *request) {
    if (!buffer->active) return;
    
    sem_wait(buffer->empty);
    
    if (!buffer->active) {
        sem_post(buffer->empty);
        return;
    }
    
    pthread_mutex_lock(&buffer->mutex);
    
    buffer->requests[buffer->in] = *request;
    buffer->in = (buffer->in + 1) % BUFFER_SIZE;
    
    pthread_mutex_unlock(&buffer->mutex);
    sem_post(buffer->full);
}

// Remover pedido do buffer (consumidor)
int buffer_remove(connection_buffer_t *buffer, connection_request_t *request) {
    if (!buffer->active) return -1;
    
    // macOS não tem sem_timedwait, usar sem_trywait com sleep
    int attempts = 0;
    while (attempts < 10) {
        if (!buffer->active) return -1;
        
        if (sem_trywait(buffer->full) == 0) {
            break;
        }
        
        if (errno != EAGAIN) {
            return -1;
        }
        
        sleep_ms(100); // 100ms
        attempts++;
    }
    
    if (attempts >= 10) {
        return -1; // timeout
    }
    
    if (!buffer->active) {
        sem_post(buffer->full);
        return -1;
    }
    
    pthread_mutex_lock(&buffer->mutex);
    
    *request = buffer->requests[buffer->out];
    buffer->out = (buffer->out + 1) % BUFFER_SIZE;
    
    pthread_mutex_unlock(&buffer->mutex);
    sem_post(buffer->empty);
    
    return 0;
}

// Carregar próximo nível para a sessão
int load_next_level(session_t *sess) {
    // Listar todos os ficheiros .lvl
    DIR* level_dir = opendir(levels_dir);
    if (level_dir == NULL) {
        debug("Session %d: Failed to open levels directory\n", sess->session_id);
        return -1;
    }
    
    // Criar array de nomes de ficheiros de níveis
    char level_files[100][256];
    int num_levels = 0;
    
    struct dirent* entry;
    while ((entry = readdir(level_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char *dot = strrchr(entry->d_name, '.');
        if (dot && strcmp(dot, ".lvl") == 0) {
            strncpy(level_files[num_levels], entry->d_name, sizeof(level_files[0]) - 1);
            num_levels++;
            if (num_levels >= 100) break;
        }
    }
    closedir(level_dir);
    
    // Ordenar os ficheiros (para garantir ordem consistente)
    for (int i = 0; i < num_levels - 1; i++) {
        for (int j = i + 1; j < num_levels; j++) {
            if (strcmp(level_files[i], level_files[j]) > 0) {
                char temp[256];
                strcpy(temp, level_files[i]);
                strcpy(level_files[i], level_files[j]);
                strcpy(level_files[j], temp);
            }
        }
    }
    
    debug("Session %d: Found %d levels, current level: %d\n", sess->session_id, num_levels, sess->current_level);
    
    // Verificar se há próximo nível
    if (sess->current_level >= num_levels) {
        debug("Session %d: No more levels - VICTORY!\n", sess->session_id);
        return -1; // Não há mais níveis
    }
    
    // Descarregar nível atual
    if (sess->board != NULL) {
        unload_level(sess->board);
    } else {
        sess->board = malloc(sizeof(board_t));
    }
    
    // Carregar próximo nível
    debug("Session %d: Loading level %s (index %d)\n", sess->session_id, level_files[sess->current_level], sess->current_level);
    if (load_level(sess->board, level_files[sess->current_level], levels_dir, 0) != 0) {
        debug("Session %d: Failed to load level %s\n", sess->session_id, level_files[sess->current_level]);
        return -1;
    }
    
    return 0;
}

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        server_running = 0;
    }
}

void send_board_update(session_t *sess) {
    if (!sess->board || sess->notif_fd == -1) return;
    
    board_t *board = sess->board;
    
    // Preparar mensagem
    char msg[8192];
    int offset = 0;
    
    // OP_CODE
    msg[offset++] = OP_CODE_BOARD;
    
    // Dimensões e estado
    memcpy(msg + offset, &board->width, sizeof(int));
    offset += sizeof(int);
    memcpy(msg + offset, &board->height, sizeof(int));
    offset += sizeof(int);
    memcpy(msg + offset, &board->tempo, sizeof(int));
    offset += sizeof(int);
    
    // Victory (chegou ao portal)
    int victory = sess->victory;
    memcpy(msg + offset, &victory, sizeof(int));
    offset += sizeof(int);
    
    // Game over (pacman morreu)
    int game_over = 0;
    if (board->n_pacmans > 0 && !board->pacmans[0].alive) {
        game_over = 1;
    }
    memcpy(msg + offset, &game_over, sizeof(int));
    offset += sizeof(int);
    
    // Pontos acumulados
    int accumulated_points = 0;
    if (board->n_pacmans > 0) {
        accumulated_points = board->pacmans[0].points;
    }
    memcpy(msg + offset, &accumulated_points, sizeof(int));
    offset += sizeof(int);
    
    // Dados do tabuleiro (converter para formato do cliente)
    int board_size = board->width * board->height;
    for (int i = 0; i < board_size; i++) {
        char ch = board->board[i].content;
        
        // Converter caracteres internos para formato de display
        switch (ch) {
            case 'W': // Wall
                msg[offset++] = '#';
                break;
            case 'P': // Pacman
                msg[offset++] = 'C';
                break;
            case 'M': // Monster/Ghost
                msg[offset++] = 'M';
                break;
            case ' ': // Empty space
                if (board->board[i].has_portal) {
                    msg[offset++] = '@';
                } else if (board->board[i].has_dot) {
                    msg[offset++] = '.';
                } else {
                    msg[offset++] = ' ';
                }
                break;
            default:
                msg[offset++] = ch;
                break;
        }
    }
    
    // Enviar
    write(sess->notif_fd, msg, offset);
}

// Thread para enviar updates periódicos
void* update_sender(void* arg) {
    session_t *sess = (session_t*)arg;
    
    while (sess->game_active) {
        pthread_mutex_lock(&sess->session_lock);
        if (!sess->game_active || sess->board == NULL) {
            pthread_mutex_unlock(&sess->session_lock);
            break;
        }
        int tempo = sess->board->tempo;
        pthread_mutex_unlock(&sess->session_lock);
        
        sleep_ms(tempo);
        
        pthread_mutex_lock(&sess->session_lock);
        if (sess->game_active && sess->notif_fd != -1) {
            board_t *board = sess->board;
            
            pthread_rwlock_wrlock(&board->state_lock);
            
            // Processar movimentos automáticos do Pacman (se tiver movimentos definidos)
            if (board->n_pacmans > 0 && board->pacmans[0].n_moves > 0) {
                pacman_t *pacman = &board->pacmans[0];
                if (pacman->alive) {
                    int result = move_pacman(board, 0, &pacman->moves[pacman->current_move % pacman->n_moves]);
                    
                    if (result == REACHED_PORTAL) {
                        debug("Session %d: Pacman reached portal!\n", sess->session_id);
                        pthread_rwlock_unlock(&board->state_lock);
                        
                        // Tentar carregar próximo nível
                        sess->current_level++;
                        if (load_next_level(sess) != 0) {
                            // Não há mais níveis - vitória!
                            sess->victory = 1;
                            send_board_update(sess);
                            sess->game_active = 0;
                        } else {
                            // Nível carregado com sucesso
                            send_board_update(sess);
                        }
                        pthread_mutex_unlock(&sess->session_lock);
                        continue;
                    } else if (result == DEAD_PACMAN) {
                        debug("Session %d: Pacman died!\n", sess->session_id);
                        // Enviar update final para cliente ver game over
                        pthread_rwlock_unlock(&board->state_lock);
                        send_board_update(sess);
                        sess->game_active = 0;
                        pthread_mutex_unlock(&sess->session_lock);
                        continue;
                    }
                }
            }
            
            // Processar movimentos dos monstros
            for (int i = 0; i < board->n_ghosts; i++) {
                ghost_t *ghost = &board->ghosts[i];
                if (ghost->n_moves > 0) {
                    move_ghost(board, i, &ghost->moves[ghost->current_move % ghost->n_moves]);
                }
            }
            
            pthread_rwlock_unlock(&board->state_lock);
            
            if (sess->game_active && sess->board != NULL) {
                send_board_update(sess);
            }
        }
        pthread_mutex_unlock(&sess->session_lock);
    }
    
    return NULL;
}

// Thread anfitriã - recebe pedidos de conexão e insere no buffer
void* host_thread(void* arg) {
    (void)arg;
    
    debug("Host thread started\n");
    
    // Abrir FIFO de registro
    int registry_fd = open(registry_pipe, O_RDONLY);
    if (registry_fd == -1) {
        debug("Failed to open registry pipe: %s\n", strerror(errno));
        return NULL;
    }
    
    debug("Registry pipe opened in host thread\n");
    
    while (server_running) {
        char buffer[256];
        ssize_t bytes_read = read(registry_fd, buffer, sizeof(buffer));
        
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                // EOF - reabrir pipe
                close(registry_fd);
                registry_fd = open(registry_pipe, O_RDONLY);
                continue;
            }
            break;
        }
        
        // Parsear mensagem de conexão
        if (buffer[0] != OP_CODE_CONNECT) {
            debug("Invalid operation code in connect message\n");
            continue;
        }
        
        connection_request_t request;
        memcpy(request.req_pipe_path, buffer + 1, MAX_PIPE_PATH_LENGTH);
        memcpy(request.notif_pipe_path, buffer + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
        
        request.req_pipe_path[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        request.notif_pipe_path[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        
        debug("Connection request received: req=%s notif=%s\n", 
              request.req_pipe_path, request.notif_pipe_path);
        
        // Inserir no buffer produtor-consumidor
        buffer_insert(&conn_buffer, &request);
        debug("Connection request inserted into buffer\n");
    }
    
    close(registry_fd);
    debug("Host thread ended\n");
    return NULL;
}

// Thread gestora - processa sessões de clientes
void* manager_thread(void* arg) {
    int manager_id = *(int*)arg;
    free(arg);
    
    debug("Manager thread %d started\n", manager_id);
    
    while (server_running) {
        connection_request_t request;
        
        // Tentar obter pedido do buffer
        if (buffer_remove(&conn_buffer, &request) != 0) {
            continue; // timeout, tentar novamente
        }
        
        debug("Manager %d: Got connection request\n", manager_id);
        
        // Encontrar slot de sessão livre
        int session_id = -1;
        for (int i = 0; i < max_games; i++) {
            pthread_mutex_lock(&sessions[i].session_lock);
            if (!sessions[i].active) {
                session_id = i;
                sessions[i].active = 1;
                sessions[i].session_id = i;
                sessions[i].game_active = 1;
                sessions[i].victory = 0;
                sessions[i].current_level = 0;
                strncpy(sessions[i].req_pipe_path, request.req_pipe_path, MAX_PIPE_PATH_LENGTH);
                strncpy(sessions[i].notif_pipe_path, request.notif_pipe_path, MAX_PIPE_PATH_LENGTH);
                pthread_mutex_unlock(&sessions[i].session_lock);
                break;
            }
            pthread_mutex_unlock(&sessions[i].session_lock);
        }
        
        if (session_id == -1) {
            debug("Manager %d: No free session slots\n", manager_id);
            continue;
        }
        
        debug("Manager %d: Using session slot %d\n", manager_id, session_id);
        
        session_t *sess = &sessions[session_id];
        
        // Carregar primeiro nível para esta sessão
        sess->board = NULL; // será alocado em load_next_level
        if (load_next_level(sess) != 0) {
            debug("Manager %d: Failed to load first level\n", manager_id);
            pthread_mutex_lock(&sess->session_lock);
            sess->active = 0;
            pthread_mutex_unlock(&sess->session_lock);
            continue;
        }
        
        // Processar a sessão (executar session_handler inline)
        session_handler(sess);
    }
    
    debug("Manager thread %d ended\n", manager_id);
    return NULL;
}

// Thread da sessão
void* session_handler(void* arg) {
    session_t *sess = (session_t*)arg;
    
    debug("Session %d handler started\n", sess->session_id);
    
    // Abrir FIFOs do cliente
    sess->req_fd = open(sess->req_pipe_path, O_RDONLY);
    if (sess->req_fd == -1) {
        debug("Failed to open request pipe: %s\n", sess->req_pipe_path);
        sess->active = 0;
        return NULL;
    }
    
    sess->notif_fd = open(sess->notif_pipe_path, O_WRONLY);
    if (sess->notif_fd == -1) {
        debug("Failed to open notification pipe: %s\n", sess->notif_pipe_path);
        close(sess->req_fd);
        sess->active = 0;
        return NULL;
    }
    
    debug("Pipes opened successfully\n");
    
    // Enviar primeira atualização
    pthread_mutex_lock(&sess->session_lock);
    send_board_update(sess);
    pthread_mutex_unlock(&sess->session_lock);
    
    // Iniciar thread de updates periódicos
    pthread_create(&sess->update_thread, NULL, update_sender, sess);
    
    // Loop de processamento de comandos
    char buffer[256];
    while (sess->game_active) {
        ssize_t bytes_read = read(sess->req_fd, buffer, sizeof(buffer));
        
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                debug("Client disconnected (EOF)\n");
            } else {
                debug("Error reading from pipe: %s\n", strerror(errno));
            }
            break;
        }
        
        char op_code = buffer[0];
        
        pthread_mutex_lock(&sess->session_lock);
        
        switch (op_code) {
            case OP_CODE_DISCONNECT: {
                debug("Session %d: Received disconnect request\n", sess->session_id);
                sess->game_active = 0;
                
                // Enviar resposta
                char response[2];
                response[0] = OP_CODE_DISCONNECT;
                response[1] = 0; // Sucesso
                write(sess->notif_fd, response, 2);
                break;
            }
            
            case OP_CODE_PLAY: {
                if (bytes_read >= 2) {
                    char command = buffer[1];
                    debug("Session %d: Received play command: %c\n", sess->session_id, command);
                    
                    // Só processar comandos manuais se o Pacman não tiver movimentos automáticos
                    if (sess->board && sess->board->n_pacmans > 0) {
                        board_t *board = sess->board;
                        
                        // Se o Pacman tem movimentos definidos no ficheiro, ignorar comandos manuais
                        if (board->pacmans[0].n_moves > 0) {
                            debug("Pacman has automatic moves, ignoring manual command\n");
                            break;
                        }
                        
                        command_t cmd;
                        cmd.command = command;
                        cmd.turns = 1;
                        cmd.turns_left = 1;
                        
                        pthread_rwlock_wrlock(&board->state_lock);
                        int result = move_pacman(board, 0, &cmd);
                        pthread_rwlock_unlock(&board->state_lock);
                        
                        // Verificar se chegou ao portal ou morreu
                        if (result == REACHED_PORTAL) {
                            debug("Session %d: Pacman reached portal (manual)!\n", sess->session_id);
                            
                            // Tentar carregar próximo nível
                            sess->current_level++;
                            if (load_next_level(sess) != 0) {
                                // Não há mais níveis - vitória!
                                sess->victory = 1;
                                send_board_update(sess);
                                sess->game_active = 0;
                            } else {
                                // Nível carregado com sucesso
                                send_board_update(sess);
                            }
                        } else if (result == DEAD_PACMAN) {
                            debug("Session %d: Pacman died (manual)!\n", sess->session_id);
                            // Enviar update final para cliente ver game over
                            send_board_update(sess);
                            sess->game_active = 0;
                        } else {
                            // Enviar update normal
                            send_board_update(sess);
                        }
                    }
                }
                break;
            }
            
            default:
                debug("Unknown operation code: %d\n", op_code);
                break;
        }
        
        pthread_mutex_unlock(&sess->session_lock);
    }
    
    // Aguardar thread de updates
    pthread_join(sess->update_thread, NULL);
    
    // Cleanup
    debug("Session %d: Cleaning up resources\n", sess->session_id);
    if (sess->req_fd != -1) {
        close(sess->req_fd);
        sess->req_fd = -1;
    }
    if (sess->notif_fd != -1) {
        close(sess->notif_fd);
        sess->notif_fd = -1;
    }
    if (sess->board != NULL) {
        unload_level(sess->board);
        free(sess->board);
        sess->board = NULL;
    }
    
    pthread_mutex_lock(&sess->session_lock);
    sess->active = 0;
    pthread_mutex_unlock(&sess->session_lock);
    
    debug("Session %d ended\n", sess->session_id);
    
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s levels_dir max_games nome_do_FIFO_de_registo\n", argv[0]);
        return 1;
    }
    
    max_games = atoi(argv[2]);
    
    strncpy(levels_dir, argv[1], sizeof(levels_dir) - 1);
    strncpy(registry_pipe, argv[3], sizeof(registry_pipe) - 1);
    
    if (max_games <= 0) {
        fprintf(stderr, "max_games deve ser maior que 0\n");
        return 1;
    }
    
    // Inicializar debug
    open_debug_file("server_debug.log");
    debug("Starting PacmanIST server (Etapa 1.2)\n");
    debug("Levels dir: %s\n", levels_dir);
    debug("Max games: %d\n", max_games);
    debug("Registry pipe: %s\n", registry_pipe);
    
    // Alocar array de sessões
    sessions = calloc(max_games, sizeof(session_t));
    if (sessions == NULL) {
        fprintf(stderr, "Erro ao alocar memória para sessões\n");
        return 1;
    }
    
    // Inicializar sessões
    for (int i = 0; i < max_games; i++) {
        sessions[i].active = 0;
        sessions[i].session_id = i;
        sessions[i].req_fd = -1;
        sessions[i].notif_fd = -1;
        sessions[i].board = NULL;
        sessions[i].victory = 0;
        sessions[i].current_level = 0;
        pthread_mutex_init(&sessions[i].session_lock, NULL);
    }
    
    // Inicializar buffer produtor-consumidor
    init_connection_buffer(&conn_buffer);
    
    // Criar FIFO de registro
    unlink(registry_pipe);
    if (mkfifo(registry_pipe, 0666) == -1) {
        debug("Failed to create registry pipe: %s\n", strerror(errno));
        fprintf(stderr, "Erro ao criar FIFO de registro: %s\n", strerror(errno));
        return 1;
    }
    
    debug("Registry FIFO created: %s\n", registry_pipe);
    
    // Configurar handlers de sinais
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    debug("Starting host thread and manager threads\n");
    
    // Criar thread anfitriã
    pthread_t host_tid;
    pthread_create(&host_tid, NULL, host_thread, NULL);
    
    // Criar threads gestoras (max_games threads)
    pthread_t *manager_tids = malloc(max_games * sizeof(pthread_t));
    for (int i = 0; i < max_games; i++) {
        int *manager_id = malloc(sizeof(int));
        *manager_id = i;
        pthread_create(&manager_tids[i], NULL, manager_thread, manager_id);
    }
    
    debug("All threads created, server is running\n");
    
    // Aguardar sinal de terminação
    pthread_join(host_tid, NULL);
    
    debug("Host thread joined, waiting for managers\n");
    
    // Aguardar threads gestoras
    for (int i = 0; i < max_games; i++) {
        pthread_join(manager_tids[i], NULL);
    }
    
    free(manager_tids);
    
    // Cleanup
    debug("Shutting down server\n");
    
    // Limpar todas as sessões
    for (int i = 0; i < max_games; i++) {
        pthread_mutex_lock(&sessions[i].session_lock);
        if (sessions[i].active) {
            sessions[i].game_active = 0;
            if (sessions[i].req_fd != -1) close(sessions[i].req_fd);
            if (sessions[i].notif_fd != -1) close(sessions[i].notif_fd);
            if (sessions[i].board != NULL) {
                unload_level(sessions[i].board);
                free(sessions[i].board);
            }
        }
        pthread_mutex_unlock(&sessions[i].session_lock);
        pthread_mutex_destroy(&sessions[i].session_lock);
    }
    
    free(sessions);
    
    // Destruir buffer
    destroy_connection_buffer(&conn_buffer);
    
    unlink(registry_pipe);
    
    close_debug_file();
    
    return 0;
}