# Caminho para o FIFO e níveis
LEVELS_DIR=../levels
REGISTRY_PIPE=/tmp/pacmanist_server
MAX_GAMES=1

### (Removido bloco duplicado de run-server)
# Compiler variables
CC = gcc
CFLAGS = -g -Wall -Wextra -Werror -std=c17 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncurses

# Directory variables
SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INCLUDE_DIR = include

# executables 
TARGET = Pacmanist
SERVER_TARGET = PacmanistServer

# Objects variables
OBJS = game.o display.o board.o parser.o
OBJS_SERVER = server.o board.o parser.o

# Dependencies
display.o = display.h
board.o = board.h
parser.o = parser.h
server.o = board.h

# Object files path
vpath %.o $(OBJ_DIR)
vpath %.c $(SRC_DIR)

# Make targets
all: pacmanist pacmanist-server

pacmanist: $(BIN_DIR)/$(TARGET)

pacmanist-server: $(BIN_DIR)/$(SERVER_TARGET)

$(BIN_DIR)/$(TARGET): $(OBJS) | folders
	$(CC) $(CFLAGS) $(SLEEP) $(addprefix $(OBJ_DIR)/,$(OBJS)) -o $@ $(LDFLAGS)

$(BIN_DIR)/$(SERVER_TARGET): $(OBJS_SERVER) | folders
	$(CC) $(CFLAGS) $(addprefix $(OBJ_DIR)/,$(OBJS_SERVER)) -o $@

# dont include LDFLAGS in the end, to allow compilation on macos
%.o: %.c $($@) | folders
	$(CC) -I $(INCLUDE_DIR) $(CFLAGS) -o $(OBJ_DIR)/$@ -c $<

# Create folders
folders:
	mkdir -p $(OBJ_DIR)
	mkdir -p $(BIN_DIR)

# Clean object files and executable
clean:
	rm -f $(OBJ_DIR)/*.o
	rm -f $(BIN_DIR)/$(TARGET)
	rm -f $(BIN_DIR)/$(SERVER_TARGET)

# indentify targets that do not create files
.PHONY: all clean folders
