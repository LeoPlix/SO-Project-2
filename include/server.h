#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include "board.h" 
#include "protocol.h"

#define BUFFER_SIZE 10

typedef struct {
    int active;                     
    int session_id;              
    
    // Named Pipe Paths
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];   // Client -> Server
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH]; // Server -> Client
    
    // File Descriptors
    int req_fd;
    int notif_fd;
    
    // Game State
    board_t *board;                 
    int game_active;             
    int victory;              
    int current_level;        
    
    // Threading
    pthread_t update_thread;        // Thread responsible for ghost movement/updates
    pthread_mutex_t session_lock;   // Mutex protecting access to this specific session's data
} session_t;


typedef struct {
    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
} connection_request_t;

typedef struct {
    connection_request_t requests[BUFFER_SIZE];
    int in;                         // Write index (Producer)
    int out;                        // Read index (Consumer)
    
    // Synchronization
    sem_t *empty;                   // Semaphore counting available slots (initial value = BUFFER_SIZE)
    sem_t *full;                    // Semaphore counting items to consume (initial value = 0)
    pthread_mutex_t mutex;          // Mutex for exclusive access to the buffer indices
    
    int active;        
} connection_buffer_t;

// Flags accessed by signal handlers and main loop
extern _Atomic int server_running;
extern _Atomic int sigusr1_received;

// Configuration and Resources
extern char registry_pipe[MAX_PIPE_PATH_LENGTH];
extern session_t *sessions;
extern int max_games;
extern connection_buffer_t conn_buffer;
extern char levels_dir[256];
extern int shutdown_pipe[2];

// --- Connection Buffer Management ---
void init_connection_buffer(connection_buffer_t *buffer);
void destroy_connection_buffer(connection_buffer_t *buffer);
void cleanup_connection_resources(connection_buffer_t *buffer);
void buffer_insert(connection_buffer_t *buffer, connection_request_t *request);
int buffer_remove(connection_buffer_t *buffer, connection_request_t *request);

// --- Server Logic & Helpers ---
void signal_handler(int signum);
void send_board_update(session_t *sess);
int load_next_level(session_t *sess);
void generate_top5_file();                       // Generates the scoreboard file
void free_session_resources(session_t *sess);    
int handle_move_result(session_t *sess, int result); // Processes the outcome of a move

// --- Thread Entry Points ---
void* update_sender(void* arg);  
void* session_handler(void* arg); 
void* host_thread(void* arg);     // Producer
void* manager_thread(void* arg);  // Consumer

#endif // SERVER_H