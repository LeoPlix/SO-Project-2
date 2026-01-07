pacmanist: $(BIN_DIR)/$(TARGET)
pacmanist-server: $(BIN_DIR)/$(SERVER_TARGET)
pacmanist: $(BIN_DIR)/$(TARGET)
pacmanist-server: $(BIN_DIR)/$(SERVER_TARGET)

# Pacmanist Makefile (estrutura src/client, src/server, src/common)

# Diretórios
SRC_DIR := src
OBJ_DIR := obj
BIN_DIR := bin
INCLUDE_DIR := include
CLIENT_DIR := $(SRC_DIR)/client
SERVER_DIR := $(SRC_DIR)/server
COMMON_DIR := $(SRC_DIR)/common

# Executáveis
CLIENT_TARGET := client
SERVER_TARGET := PacmanIST

# Fontes
CLIENT_SRCS := $(CLIENT_DIR)/client_main.c $(CLIENT_DIR)/api.c $(CLIENT_DIR)/debug.c $(CLIENT_DIR)/display.c
SERVER_SRCS := $(SERVER_DIR)/server.c $(CLIENT_DIR)/debug.c
COMMON_SRCS := $(filter-out $(COMMON_DIR)/debug.c,$(wildcard $(COMMON_DIR)/*.c))

# Objetos
CLIENT_OBJS := $(patsubst $(CLIENT_DIR)/%.c,$(OBJ_DIR)/client_%.o,$(CLIENT_SRCS))
SERVER_OBJS := $(patsubst $(SERVER_DIR)/%.c,$(OBJ_DIR)/server_%.o,$(SERVER_SRCS))
COMMON_OBJS := $(patsubst $(COMMON_DIR)/%.c,$(OBJ_DIR)/common_%.o,$(COMMON_SRCS))

# Flags
CC := gcc
CFLAGS := -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L -I$(INCLUDE_DIR) -fsanitize=thread
LDFLAGS := -lncurses -fsanitize=thread

# Alvos principais
all: folders $(BIN_DIR)/$(CLIENT_TARGET) $(BIN_DIR)/$(SERVER_TARGET)

$(BIN_DIR)/$(CLIENT_TARGET): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(BIN_DIR)/$(SERVER_TARGET): $(SERVER_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Compilação dos objetos
$(OBJ_DIR)/client_%.o: $(CLIENT_DIR)/%.c | folders
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/server_%.o: $(SERVER_DIR)/%.c | folders
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/common_%.o: $(COMMON_DIR)/%.c | folders
	$(CC) $(CFLAGS) -c $< -o $@

# Criação de diretórios
folders:
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(BIN_DIR)

# Limpeza
clean:
	rm -rf $(OBJ_DIR)/* $(BIN_DIR)/$(CLIENT_TARGET) $(BIN_DIR)/$(SERVER_TARGET)

.PHONY: all clean folders
