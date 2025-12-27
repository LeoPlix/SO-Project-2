#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>

#define MAX_PIPE_PATH_LENGTH 40

// Códigos de operação
enum {
    OP_CODE_CONNECT = 1,
    OP_CODE_DISCONNECT = 2,
    OP_CODE_PLAY = 3,
    OP_CODE_BOARD = 4,
};

// Estrutura da sessão
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

extern int server_running;
extern char registry_pipe[MAX_PIPE_PATH_LENGTH];
extern session_t session;

void signal_handler(int signum);
void send_board_update(session_t *sess);
void* update_sender(void* arg);
void* session_handler(void* arg);

#endif // SERVER_H
