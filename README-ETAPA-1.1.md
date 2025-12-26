# PacmanIST - Etapa 1.1

## Descrição
Implementação da etapa 1.1 do projeto de Sistemas Operativos 2025-26.
Esta etapa implementa um servidor PacmanIST com **sessão única** que permite a clientes conectarem-se via **named pipes** e jogarem Pacman remotamente.

## Estrutura do Projeto

### Servidor (`SO-2526-sol-parte1/`)
- **Código fonte**: `src/server.c`
- **Executável**: `bin/PacmanistServer`
- **Dependências**: Usa `board.c`, `parser.c` existentes da parte 1

### Cliente (`client-base-with-Makefile-v3/`)
- **Código fonte**: `src/client/api.c`, `src/client/client_main.c`
- **Executável**: `bin/client`
- **API implementada**: `pacman_connect()`, `pacman_disconnect()`, `pacman_play()`, `receive_board_update()`

## Compilação

### Compilar o Servidor
```bash
cd SO-2526-sol-parte1
make pacmanist-server
```

### Compilar o Cliente
```bash
cd client-base-with-Makefile-v3
make client
```

## Execução

### 1. Iniciar o Servidor
```bash
cd SO-2526-sol-parte1
./bin/PacmanistServer <levels_dir> <max_games> <nome_do_FIFO_de_registo>
```

**Exemplo:**
```bash
./bin/PacmanistServer ../levels 1 /tmp/pacmanist_registry
```

**Parâmetros:**
- `levels_dir`: Diretório contendo os ficheiros `.lvl`
- `max_games`: Número máximo de jogos simultâneos (1 para etapa 1.1)
- `nome_do_FIFO_de_registo`: Nome do FIFO para registro de clientes

### 2. Iniciar o Cliente
```bash
cd client-base-with-Makefile-v3
./bin/client <client_id> <register_pipe> [commands_file]
```

**Exemplo (modo interativo):**
```bash
./bin/client cliente1 /tmp/pacmanist_registry
```

**Exemplo (com ficheiro de comandos):**
```bash
./bin/client cliente1 /tmp/pacmanist_registry comandos.txt
```

**Parâmetros:**
- `client_id`: Identificador único do cliente
- `register_pipe`: Nome do FIFO de registro do servidor
- `commands_file`: (Opcional) Ficheiro com comandos automáticos

## Protocolo de Comunicação

### Named Pipes
O cliente cria dois FIFOs:
- **FIFO de Pedidos**: `/tmp/<client_id>_request` - Cliente → Servidor
- **FIFO de Notificações**: `/tmp/<client_id>_notification` - Servidor → Cliente

### Mensagens

#### 1. Conexão (OP_CODE_CONNECT = 1)
Cliente envia para FIFO de registro:
```
[1 byte: OP_CODE] [40 bytes: req_pipe_path] [40 bytes: notif_pipe_path] [40 bytes: server_pipe_path]
```

#### 2. Desconexão (OP_CODE_DISCONNECT = 2)
Cliente → Servidor:
```
[1 byte: OP_CODE]
```
Servidor → Cliente:
```
[1 byte: OP_CODE] [1 byte: resultado (0=sucesso)]
```

#### 3. Jogada (OP_CODE_PLAY = 3)
Cliente → Servidor:
```
[1 byte: OP_CODE] [1 byte: comando (W/A/S/D/T/R)]
```

#### 4. Atualização do Tabuleiro (OP_CODE_BOARD = 4)
Servidor → Cliente (periódico):
```
[1 byte: OP_CODE]
[4 bytes: width]
[4 bytes: height]
[4 bytes: tempo]
[4 bytes: victory]
[4 bytes: game_over]
[4 bytes: accumulated_points]
[width*height bytes: board_data]
```

## Comandos do Jogo
- **W**: Mover para cima
- **A**: Mover para a esquerda
- **S**: Mover para baixo
- **D**: Mover para a direita
- **T**: Esperar (não mover)
- **R**: Movimento aleatório
- **Q**: Sair do jogo

## Arquitetura (Etapa 1.1)

```
┌─────────────────────────────────────┐
│     Servidor PacmanIST              │
│  - Sessão única                     │
│  - Gestão do tabuleiro (board.c)    │
│  - Thread de updates periódicos     │
│  - FIFO de registro                 │
└──────────┬──────────────────────────┘
           │
           │ Named Pipes
           │
┌──────────▼──────────────────────────┐
│     Cliente                          │
│  - FIFO de pedidos                  │
│  - FIFO de notificações             │
│  - Thread de receção de updates     │
│  - Interface ncurses                │
└─────────────────────────────────────┘
```

## Características da Etapa 1.1

✅ Servidor aceita **apenas uma sessão** por vez
✅ Cliente liga-se ao servidor via **named pipes**
✅ Servidor envia **updates periódicos** do tabuleiro
✅ Cliente pode enviar **comandos** para mover o Pacman
✅ Usa o código existente de `board.c` para lógica do jogo
✅ **Sem modificação** do código da parte 1 (reutilização total)

## Ficheiros de Debug
- Servidor: `server_debug.log`
- Cliente: `client-debug.log`

## Limitações da Etapa 1.1
- Apenas **1 sessão simultânea** (será expandido na etapa 1.2)
- Sem gestão de múltiplos níveis (apenas carrega o primeiro `.lvl`)
- Sem threads de fantasmas no servidor (simplificação)
- O jogo termina quando o Pacman morre ou chega ao portal

## Estrutura de Código Reutilizada
A implementação aproveita integralmente:
- `board.h` / `board.c`: Toda a lógica do tabuleiro, movimentos, colisões
- `parser.h` / `parser.c`: Carregamento de níveis
- `display.h` / `display.c`: Interface ncurses (cliente)
- `debug.h` / `debug.c`: Sistema de logging

## Próximos Passos (Etapa 1.2)
- Suporte a **múltiplas sessões** simultâneas
- Thread por sessão para gestão independente
- Sistema de fila para sessões pendentes quando servidor está cheio
