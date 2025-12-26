#include "board.h"
#include "display.h"
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

#define MAX_PIPE_PATH_LENGTH 40

// Códigos de operação
enum {
    OP_CODE_CONNECT = 1,
    OP_CODE_DISCONNECT = 2,
    OP_CODE_PLAY = 3,
    OP_CODE_BOARD = 4,
};

// Estrutura da sessão (etapa 1.1: apenas uma sessão)
typedef struct {
    int active;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
    int req_fd;
    int notif_fd;
    board_t *board;
    pthread_t update_thread;
    pthread_mutex_t session_lock;
    int game_active;
} session_t;

session_t session = {0};
int server_running = 1;
char registry_pipe[MAX_PIPE_PATH_LENGTH];

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
    int victory = 0;
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
    
    // Dados do tabuleiro
    int board_size = board->width * board->height;
    for (int i = 0; i < board_size; i++) {
        msg[offset++] = board->board[i].content;
    }
    
    // Enviar
    write(sess->notif_fd, msg, offset);
}

// Thread para enviar updates periódicos
void* update_sender(void* arg) {
    session_t *sess = (session_t*)arg;
    
    while (sess->game_active) {
        sleep_ms(sess->board->tempo);
        
        pthread_mutex_lock(&sess->session_lock);
        if (sess->game_active && sess->notif_fd != -1) {
            send_board_update(sess);
        }
        pthread_mutex_unlock(&sess->session_lock);
    }
    
    return NULL;
}

// Thread da sessão
void* session_handler(void* arg) {
    session_t *sess = (session_t*)arg;
    
    debug("Session handler started\n");
    
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
                debug("Received disconnect request\n");
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
                    debug("Received play command: %c\n", command);
                    
                    // Processar comando
                    if (sess->board && sess->board->n_pacmans > 0) {
                        command_t cmd;
                        cmd.command = command;
                        cmd.turns = 1;
                        cmd.turns_left = 1;
                        
                        pthread_rwlock_rdlock(&sess->board->state_lock);
                        int result = move_pacman(sess->board, 0, &cmd);
                        pthread_rwlock_unlock(&sess->board->state_lock);
                        
                        // Enviar update imediato
                        send_board_update(sess);
                        
                        // Verificar se chegou ao portal ou morreu
                        if (result == REACHED_PORTAL) {
                            debug("Pacman reached portal!\n");
                            sess->game_active = 0;
                        } else if (result == DEAD_PACMAN) {
                            debug("Pacman died!\n");
                            sess->game_active = 0;
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
    
    sess->active = 0;
    debug("Session ended\n");
    
    return NULL;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s levels_dir max_games nome_do_FIFO_de_registo\n", argv[0]);
        return 1;
    }
    
    char levels_dir[256];
    int max_games = atoi(argv[2]);
    
    strncpy(levels_dir, argv[1], sizeof(levels_dir) - 1);
    strncpy(registry_pipe, argv[3], sizeof(registry_pipe) - 1);
    
    if (max_games <= 0) {
        fprintf(stderr, "max_games deve ser maior que 0\n");
        return 1;
    }
    
    // Inicializar debug
    open_debug_file("server_debug.log");
    debug("Starting PacmanIST server\n");
    debug("Levels dir: %s\n", levels_dir);
    debug("Max games: %d\n", max_games);
    debug("Registry pipe: %s\n", registry_pipe);
    
    // Inicializar sessão
    session.active = 0;
    session.req_fd = -1;
    session.notif_fd = -1;
    pthread_mutex_init(&session.session_lock, NULL);
    
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
    
    // Abrir FIFO de registro
    debug("Waiting for clients on registry pipe...\n");
    int registry_fd = open(registry_pipe, O_RDONLY);
    if (registry_fd == -1) {
        debug("Failed to open registry pipe: %s\n", strerror(errno));
        fprintf(stderr, "Erro ao abrir FIFO de registro\n");
        unlink(registry_pipe);
        return 1;
    }
    
    debug("Registry pipe opened\n");
    
    // Loop principal
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
        
        char req_pipe[MAX_PIPE_PATH_LENGTH];
        char notif_pipe[MAX_PIPE_PATH_LENGTH];
        
        memcpy(req_pipe, buffer + 1, MAX_PIPE_PATH_LENGTH);
        memcpy(notif_pipe, buffer + 1 + MAX_PIPE_PATH_LENGTH, MAX_PIPE_PATH_LENGTH);
        
        req_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        notif_pipe[MAX_PIPE_PATH_LENGTH - 1] = '\0';
        
        debug("Connection request: req=%s notif=%s\n", req_pipe, notif_pipe);
        
        // Verificar se já existe sessão ativa (etapa 1.1: apenas uma)
        if (session.active) {
            debug("Session already active, rejecting connection\n");
            continue;
        }
        
        // Configurar sessão
        strncpy(session.req_pipe_path, req_pipe, MAX_PIPE_PATH_LENGTH);
        strncpy(session.notif_pipe_path, notif_pipe, MAX_PIPE_PATH_LENGTH);
        session.active = 1;
        session.game_active = 1;
        
        // Carregar primeiro nível
        session.board = malloc(sizeof(board_t));
        
        DIR* level_dir = opendir(levels_dir);
        if (level_dir == NULL) {
            debug("Failed to open levels directory\n");
            session.active = 0;
            free(session.board);
            session.board = NULL;
            continue;
        }
        
        struct dirent* entry;
        char level_file[512] = "";
        while ((entry = readdir(level_dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            
            char *dot = strrchr(entry->d_name, '.');
            if (dot && strcmp(dot, ".lvl") == 0) {
                strncpy(level_file, entry->d_name, sizeof(level_file) - 1);
                break;
            }
        }
        closedir(level_dir);
        
        if (level_file[0] == '\0') {
            debug("No .lvl file found in directory\n");
            session.active = 0;
            free(session.board);
            session.board = NULL;
            continue;
        }
        
        debug("Loading level: %s\n", level_file);
        if (load_level(session.board, level_file, levels_dir, 0) != 0) {
            debug("Failed to load level\n");
            session.active = 0;
            free(session.board);
            session.board = NULL;
            continue;
        }
        
        // Criar thread da sessão
        pthread_t session_thread;
        pthread_create(&session_thread, NULL, session_handler, &session);
        pthread_detach(session_thread);
        
        debug("Session created successfully\n");
    }
    
    // Cleanup
    debug("Shutting down server\n");
    
    if (session.active) {
        session.game_active = 0;
        if (session.req_fd != -1) close(session.req_fd);
        if (session.notif_fd != -1) close(session.notif_fd);
        if (session.board != NULL) {
            unload_level(session.board);
            free(session.board);
        }
    }
    
    close(registry_fd);
    unlink(registry_pipe);
    pthread_mutex_destroy(&session.session_lock);
    
    close_debug_file();
    
    return 0;
}
