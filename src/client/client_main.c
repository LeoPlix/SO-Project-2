#include "api.h"
#include "protocol.h"
#include "display.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>

Board board = {0};
bool stop_execution = false;
int tempo = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *receiver_thread(void *arg) {
    (void)arg;

    while (true) {
        
        Board new_board = receive_board_update();

        if (!new_board.data || new_board.game_over == 1){
            pthread_mutex_lock(&mutex);
            stop_execution = true;
            
            // Update the global board one last time
            if (board.data) {
                free(board.data);
            }
            board = new_board;
            pthread_mutex_unlock(&mutex);

            clear();
            refresh();

            break;
        }

        pthread_mutex_lock(&mutex);
        tempo = new_board.tempo;
        
        // Free old board data and update global board
        if (board.data) {
            free(board.data);
        }
        board = new_board;
        
        // Make a copy for drawing (we'll draw outside the lock)
        Board board_copy = board;
        board_copy.data = NULL; // Will allocate separately
        if (board.data) {
            size_t data_size = board.width * board.height;
            board_copy.data = malloc(data_size + 1);
            if (board_copy.data) {
                memcpy(board_copy.data, board.data, data_size + 1);
            }
        }
        pthread_mutex_unlock(&mutex);

        // Draw outside the lock to avoid holding it too long
        if (board_copy.data) {
            draw_board_client(board_copy);
            refresh_screen();
            free(board_copy.data);
        }
    }

    debug("Returning receiver thread...\n");
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr,
            "Usage: %s <client_id> <register_pipe> [commands_file]\n",
            argv[0]);
        return 1;
    }

    const char *client_id = argv[1];
    const char *register_pipe = argv[2];
    const char *commands_file = (argc == 4) ? argv[3] : NULL;

    FILE *cmd_fp = NULL;
    if (commands_file) {
        cmd_fp = fopen(commands_file, "r");
        if (!cmd_fp) {
            perror("Failed to open commands file");
            return 1;
        }
    }

    char req_pipe_path[MAX_PIPE_PATH_LENGTH];
    char notif_pipe_path[MAX_PIPE_PATH_LENGTH];

    snprintf(req_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_request", client_id);

    snprintf(notif_pipe_path, MAX_PIPE_PATH_LENGTH,
             "/tmp/%s_notification", client_id);

    open_debug_file("client-debug.log");

    if (pacman_connect(req_pipe_path, notif_pipe_path, register_pipe) != 0) {
        perror("Failed to connect to server");
        return 1;
    }

    pthread_t receiver_thread_id;
    pthread_create(&receiver_thread_id, NULL, receiver_thread, NULL);

    terminal_init();
    set_timeout(500);
    
    pthread_mutex_lock(&mutex);
    Board board_copy = board;
    pthread_mutex_unlock(&mutex);
    
    draw_board_client(board_copy);
    refresh_screen();

    char command;
    int ch;

    while (1) {

        pthread_mutex_lock(&mutex);
        if (stop_execution) {
            pthread_mutex_unlock(&mutex);
            break;
        }
        pthread_mutex_unlock(&mutex);

        if (cmd_fp) {
            // Input from file
            ch = fgetc(cmd_fp);

            if (ch == EOF) {
                // Restart at the start of the file
                rewind(cmd_fp);
                continue;
            }

            command = (char)ch;

            if (command == '\n' || command == '\r' || command == '\0')
                continue;

            command = toupper(command);
            
            // Wait for tempo, to not overflow pipe with requests
            pthread_mutex_lock(&mutex);
            int wait_for = tempo;
            pthread_mutex_unlock(&mutex);

            sleep_ms(wait_for);
            
        } else {
            // Interactive input
            command = get_input();
            command = toupper(command);
        }

        if (command == '\0')
            continue;

        if (command == 'Q') {
            debug("Client pressed 'Q', quitting game\n");
            break;
        }

        debug("Command: %c\n", command);

        pacman_play(command);

    }

    pacman_disconnect();

    pthread_join(receiver_thread_id, NULL);

    if (cmd_fp)
        fclose(cmd_fp);

    // Clean up board data
    pthread_mutex_lock(&mutex);
    if (board.data) {
        free(board.data);
        board.data = NULL;
    }
    pthread_mutex_unlock(&mutex);
    
    pthread_mutex_destroy(&mutex);

    terminal_cleanup();

   
    return 0;

}