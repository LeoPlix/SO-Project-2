#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include "board.h" 
#include "protocol.h"

#define BUFFER_SIZE 10

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
    int victory;     
    int current_level;  
} session_t;

// Estrutura do pedido de conexão
typedef struct {
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connection_request_t;

// Buffer Produtor-Consumidor
typedef struct {
    connection_request_t requests[BUFFER_SIZE];
    int in;  
    int out;
    sem_t *empty; // semáforo para slots vazios
    sem_t *full;  // semáforo para slots cheios
    pthread_mutex_t mutex; // mutex para acesso exclusivo
    int active;   // Flag para desativar o buffer no shutdown
} connection_buffer_t;

// Variáveis Globais (extern)
extern volatile sig_atomic_t server_running;
extern volatile sig_atomic_t sigusr1_received; // Adicionado pois é usada no server.c
extern char registry_pipe[MAX_PIPE_PATH_LENGTH];
extern session_t *sessions;
extern int max_games;
extern connection_buffer_t conn_buffer;
extern char levels_dir[256];
extern int shutdown_pipe[2];

// Protótipos de Funções de Gestão de Conexão
void init_connection_buffer(connection_buffer_t *buffer);
void destroy_connection_buffer(connection_buffer_t *buffer);
void cleanup_connection_resources(connection_buffer_t *buffer); // Faltava esta
void buffer_insert(connection_buffer_t *buffer, connection_request_t *request);
int buffer_remove(connection_buffer_t *buffer, connection_request_t *request);

// Protótipos de Funções de Lógica do Servidor
void signal_handler(int signum);
void send_board_update(session_t *sess);
int load_next_level(session_t *sess);
void generate_top5_file(); // Faltava esta (Exercício 2)
void free_session_resources(session_t *sess); // Faltava esta
int handle_move_result(session_t *sess, int result); // Faltava esta

// Protótipos das Threads
void* update_sender(void* arg);
void* session_handler(void* arg);
void* host_thread(void* arg);
void* manager_thread(void* arg);

#endif // SERVER_H