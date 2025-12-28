#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <semaphore.h>

#define MAX_PIPE_PATH_LENGTH 40
#define MAX_SESSIONS 10
#define BUFFER_SIZE 10

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
    int session_id;
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
    int req_fd;
    int notif_fd;
    board_t *board;
    pthread_t update_thread;
    pthread_mutex_t session_lock;
    int game_active;
    int victory;  // Flag para indicar vitória
    int current_level;  // Nível atual (começa em 0)
} session_t;

// Pedido de conexão no buffer
typedef struct {
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connection_request_t;

// Buffer produtor-consumidor
typedef struct {
    connection_request_t requests[BUFFER_SIZE];
    int in;  // índice de inserção
    int out; // índice de remoção
    sem_t *empty; // semáforo para slots vazios (named semaphore para macOS)
    sem_t *full;  // semáforo para slots cheios (named semaphore para macOS)
    pthread_mutex_t mutex; // mutex para acesso exclusivo
    int active; // flag para indicar se o buffer está ativo
} connection_buffer_t;

extern int server_running;
extern char registry_pipe[MAX_PIPE_PATH_LENGTH];
extern session_t *sessions;
extern int max_games;
extern connection_buffer_t conn_buffer;
extern char levels_dir[256];

void signal_handler(int signum);
void send_board_update(session_t *sess);
void* update_sender(void* arg);
void* session_handler(void* arg);
void* host_thread(void* arg);
void* manager_thread(void* arg);
void init_connection_buffer(connection_buffer_t *buffer);
void destroy_connection_buffer(connection_buffer_t *buffer);
void buffer_insert(connection_buffer_t *buffer, connection_request_t *request);
int buffer_remove(connection_buffer_t *buffer, connection_request_t *request);
int load_next_level(session_t *sess);

#endif // SERVER_H