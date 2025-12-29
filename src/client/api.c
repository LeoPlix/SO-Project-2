#include "api.h"
#include "protocol.h"
#include "debug.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>


struct Session {
  int id;
  int req_pipe;
  int notif_pipe;
  char req_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
  char notif_pipe_path[MAX_PIPE_PATH_LENGTH + 1];
};

static struct Session session = {.id = -1};

int pacman_connect(char const *req_pipe_path, char const *notif_pipe_path, char const *server_pipe_path) {
  // Guardar caminhos dos pipes
  strncpy(session.req_pipe_path, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  strncpy(session.notif_pipe_path, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  
  // Criar FIFOs do cliente
  unlink(req_pipe_path);
  unlink(notif_pipe_path);
  
  if (mkfifo(req_pipe_path, 0666) == -1) {
    return 1;
  }
  
  if (mkfifo(notif_pipe_path, 0666) == -1) {
    unlink(req_pipe_path);
    return 1;
  }
  
  // Enviar pedido de conexão ao servidor
  int server_fd = open(server_pipe_path, O_WRONLY);
  if (server_fd == -1) {
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  // Mensagem: OP_CODE_CONNECT | req_pipe_path | notif_pipe_path | server_pipe_path
  char msg[MAX_PIPE_PATH_LENGTH * 3 + 1];
  msg[0] = OP_CODE_CONNECT;
  memcpy(msg + 1, req_pipe_path, MAX_PIPE_PATH_LENGTH);
  memcpy(msg + 1 + MAX_PIPE_PATH_LENGTH, notif_pipe_path, MAX_PIPE_PATH_LENGTH);
  memcpy(msg + 1 + MAX_PIPE_PATH_LENGTH * 2, server_pipe_path, MAX_PIPE_PATH_LENGTH);
  
  // CORREÇÃO 1: Verificar retorno do write
  if (write(server_fd, msg, sizeof(msg)) == -1) {
      perror("Failed to write connect message to server");
      close(server_fd);
      unlink(req_pipe_path);
      unlink(notif_pipe_path);
      return 1;
  }

  close(server_fd);
  
  // Abrir FIFOs para comunicação (ordem correta para evitar deadlock)
  // Primeiro abrir req_pipe para escrita (servidor está à espera para ler)
  session.req_pipe = open(req_pipe_path, O_WRONLY);
  if (session.req_pipe == -1) {
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  // Depois abrir notif_pipe para leitura
  session.notif_pipe = open(notif_pipe_path, O_RDONLY);
  if (session.notif_pipe == -1) {
    close(session.req_pipe);
    unlink(req_pipe_path);
    unlink(notif_pipe_path);
    return 1;
  }
  
  return 0;
}

void pacman_play(char command) {
  if (session.req_pipe == -1) {
    return;
  }
  
  // Enviar comando: OP_CODE_PLAY | command
  char msg[2];
  msg[0] = OP_CODE_PLAY;
  msg[1] = command;
  
  // CORREÇÃO 2: Verificar retorno do write
  // Como a função é void, apenas registamos erro se falhar
  if (write(session.req_pipe, msg, 2) == -1) {
      // Se a escrita falhar, provavelmente o pipe quebrou
      perror("Failed to send play command");
  }
}

int pacman_disconnect() {
  if (session.req_pipe == -1) {
    return 1;
  }
  
  // Enviar pedido de desconexão
  char msg[1];
  msg[0] = OP_CODE_DISCONNECT;
  
  // CORREÇÃO 3: Verificar retorno do write
  if (write(session.req_pipe, msg, 1) == -1) {
      perror("Failed to send disconnect");
      // Mesmo falhando o envio, tentamos limpar os recursos abaixo
  }
  
  // Aguardar resposta
  char response[2] = {0}; // Inicializar a zero
  
  // CORREÇÃO 4: Verificar retorno do read
  if (read(session.notif_pipe, response, 2) <= 0) {
      // Se falhar a leitura ou ler 0 bytes (EOF), assumimos erro
      // mas continuamos para limpar os pipes locais
  }
  
  // Fechar pipes
  close(session.req_pipe);
  close(session.notif_pipe);
  
  // Remover FIFOs
  unlink(session.req_pipe_path);
  unlink(session.notif_pipe_path);
  
  session.req_pipe = -1;
  session.notif_pipe = -1;
  
  return response[1];
}

Board receive_board_update(void) {
  Board board = {0};
  
  if (session.notif_pipe == -1) {
    return board;
  }
  
  // Ler mensagem: OP_CODE | width | height | tempo | victory | game_over | accumulated_points | board_data
  char buffer[8192];
  ssize_t bytes_read = read(session.notif_pipe, buffer, sizeof(buffer));
  
  if (bytes_read <= 0) {
    return board;
  }
  
  int offset = 0;
  
  // OP_CODE
  char op_code = buffer[offset++];
  if (op_code != OP_CODE_BOARD) {
    return board;
  }
  
  // Ler dimensões e estado
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
  
  // Alocar e ler dados do tabuleiro
  int board_size = board.width * board.height;
  
  // Segurança básica: verificar se board_size é razoável antes de malloc
  if (board_size > 0 && board_size < 8000) { 
      board.data = malloc(board_size + 1);
      if (board.data) { // Verificar se malloc não falhou
        memcpy(board.data, buffer + offset, board_size);
        board.data[board_size] = '\0';
      }
  }
  
  return board;
}