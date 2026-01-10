#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <pthread.h>

struct Session {
  int id;
  int req_pipe;   // File descriptor for writing requests
  int notif_pipe; // File descriptor for reading notifications
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

// Session structure protected by a mutex
static struct Session session = {.id = -1};
static pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  
  // Remove pipes that might exist from previous crashed sessions
  unlink(req_pipe_path);
  unlink(notif_pipe_path);
  
  // Create FIFO for client -> server requests
  if (mkfifo(req_pipe_path, 0666) == -1) {
    return 1;
  }
  
  // Create FIFO for server -> client notifications
  if (mkfifo(notif_pipe_path, 0666) == -1) {
    unlink(req_pipe_path);
    return 1;
  }
  
  // Send connection request to the server's public FIFO
  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  // OP_CODE_CONNECT | req_pipe_path | notif_pipe_path | server_pipe_path
  char msg[MAX_PIPE_PATH_LENGTH * 3 + 1];
  msg[0] = OP_CODE_CONNECT;
  memcpy(msg + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  memcpy(msg + 1 + MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  memcpy(msg + 1 + MAX_PIPE_PATH_LENGTH * 2, server_pipe_path, MAX_PIPE_PATH_LENGTH);
  
  if (write(server_fd, msg, sizeof(msg)) == -1) {
      perror("Failed to write connect message to server");
      close(server_fd);
      unlink(req_pipe_path);
      unlink(notif_pipe_path);
      return 1;
  }

  close(server_fd);
  
  // Open FIFOs for communication
  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe == -1) {
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  // Open notification pipe for reading
  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1) {
    close(session.req_pipe);
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  // Wait for confirmation message from the server
  // The client remains blocked here until the server assigns a slot
  char confirmation[2];
  ssize_t bytes_read = read(session.notif_pipe, confirmation, 2);
  if (bytes_read != 2 || confirmation[0] != OP_CODE_CONNECT) {
    close(session.req_pipe);
    close(session.notif_pipe);
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  return 0;
}

void pacman_play(char command) {
  pthread_mutex_lock(&session_mutex);
  int req_pipe = session.req_pipe;
  pthread_mutex_unlock(&session_mutex);
  
  if (req_pipe == -1) {
    return;
  }
  
  // OP_CODE_PLAY | command_char
  char msg[2];
  msg[0] = OP_CODE_PLAY;
  msg[1] = command;

  if (write(req_pipe, msg, 2) == -1) {
      perror("Failed to send play command");
  }
}

int pacman_disconnect() {
  pthread_mutex_lock(&session_mutex);
  int req_pipe = session.req_pipe;
  int notif_pipe = session.notif_pipe;
  char req_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_path[MAX_PIPE_PATH_LENGTH + 1];
  
  // Copy paths to local stack to safely use them after unlocking
  strncpy(req_path, session.req_pipe_path, MAX_PIPE_PATH_LENGTH + 1);
  strncpy(notif_path, session.notif_pipe_path, MAX_PIPE_PATH_LENGTH + 1);
  
  if (req_pipe == -1) {
    pthread_mutex_unlock(&session_mutex);
    return 1;
  }
  pthread_mutex_unlock(&session_mutex);
  
  // Send disconnect request
  char msg[1];
  msg[0] = OP_CODE_DISCONNECT;
  
  if (write(req_pipe, msg, 1) == -1) {
      perror("Failed to send disconnect");
  }
  
  // Wait for server acknowledgement
  char response[2] = {0};
  
  if (read(notif_pipe, response, 2) <= 0) {
      // If read fails, continue anyway to clean up local resources
  }
  
  pthread_mutex_lock(&session_mutex);
  // Close file descriptors
  close(session.req_pipe);
  close(session.notif_pipe);
  
  // Remove FIFO files from filesystem
  unlink(req_path);
  unlink(notif_path);
  
  session.req_pipe = -1;
  session.notif_pipe = -1;
  pthread_mutex_unlock(&session_mutex);
  
  return response[1];
}

Board receive_board_update(void) {
  Board board = {0};
  
  pthread_mutex_lock(&session_mutex);
  int notif_pipe = session.notif_pipe;
  pthread_mutex_unlock(&session_mutex);
  
  if (notif_pipe == -1) {
    return board;
  }
  
  // OP_CODE | width | height | time | victory | game_over | accumulated_points | board_data
  char buffer[8192];
  ssize_t bytes_read = read(notif_pipe, buffer, sizeof(buffer));
  
  if (bytes_read <= 0) {
    return board;
  }
  
  int offset = 0;
  
  // Validate OpCode
  char op_code = buffer[offset++];
  if (op_code != OP_CODE_BOARD) {
    return board;
  }
  
  // Deserialize dimensions and game state
  memcpy(&board.width, buffer + offset, sizeof(int));
  offset += sizeof(int);
  memcpy(&board.height, buffer + offset, sizeof(int));
  offset += sizeof(int);
  memcpy(&board.tempo, buffer + offset, sizeof(int));
  offset += sizeof(int);
  memcpy(&board.victory, buffer + offset, sizeof(int));
  offset += sizeof(int);
  memcpy(&board.game_over, buffer + offset, sizeof(int));
  offset += sizeof(int);
  memcpy(&board.accumulated_points, buffer + offset, sizeof(int));
  offset += sizeof(int);
  
  int board_size = board.width * board.height;
  
  if (board_size > 0 && board_size < 8000) { 
      board.data = malloc(board_size + 1);
      if (board.data) { // Ensure malloc succeeded
        memcpy(board.data, buffer + offset, board_size);
        board.data[board_size] = '\0'; // Null-terminate for string safety
      }
  }
  
  return board;
}