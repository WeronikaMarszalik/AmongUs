#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include "net.h"
#include <windows.h>
#include "config.h"
#include "event_queue.h"
#include "game_state.h"
#include "protocol.h"

typedef struct Lobby {
    bool used;
    int id;
    char name[MAX_NAME_LEN + 1];
    GameState state;
    CRITICAL_SECTION mutex;
} Lobby;

typedef struct ServerContext {
    Lobby lobbies[MAX_LOBBIES];
    EventQueue queue;
    CRITICAL_SECTION lobby_mutex;
    SOCKET server_socket;
    volatile LONG running;
    int next_lobby_id;
} ServerContext;

typedef struct ClientContext {
    ServerContext *server;
    SOCKET socket_fd;
    int player_id;
    int lobby_index;
} ClientContext;

static void trim_token(char *text) {
    text[MAX_NAME_LEN] = '\0';
    for (int i = 0; text[i]; i++) {
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n') {
            text[i] = '_';
        }
    }
}

static int lobby_player_count(const Lobby *lobby) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (lobby->state.players[i].connected) count++;
    }
    return count;
}

static int find_lobby_index_by_id(ServerContext *server, int lobby_id) {
    for (int i = 0; i < MAX_LOBBIES; i++) {
        if (server->lobbies[i].used && server->lobbies[i].id == lobby_id) return i;
    }
    return -1;
}

static void send_lobby_list(ServerContext *server, SOCKET socket_fd) {
    char line[MAX_LINE_LEN * 2];
    int written = snprintf(line, sizeof(line), "LOBBIES ");

    EnterCriticalSection(&server->lobby_mutex);
    bool first = true;
    for (int i = 0; i < MAX_LOBBIES && written < (int)sizeof(line); i++) {
        if (!server->lobbies[i].used) continue;
        EnterCriticalSection(&server->lobbies[i].mutex);
        int count = lobby_player_count(&server->lobbies[i]);
        const char *phase = "LOBBY";
        if (server->lobbies[i].state.phase == PHASE_RUNNING) phase = "RUNNING";
        if (server->lobbies[i].state.phase == PHASE_MEETING) phase = "MEETING";
        if (server->lobbies[i].state.phase == PHASE_FINISHED) phase = "FINISHED";
        int added = snprintf(line + written, sizeof(line) - written, "%s%d:%s:%d/%d:%s",
                             first ? "" : "|",
                             server->lobbies[i].id,
                             server->lobbies[i].name,
                             count,
                             MAX_PLAYERS,
                             phase);
        LeaveCriticalSection(&server->lobbies[i].mutex);
        if (added < 0) break;
        written += added;
        first = false;
    }
    LeaveCriticalSection(&server->lobby_mutex);

    net_send_line(socket_fd, line);
}

static int create_lobby(ServerContext *server, const char *name) {
    int created = -1;
    EnterCriticalSection(&server->lobby_mutex);
    for (int i = 0; i < MAX_LOBBIES; i++) {
        if (!server->lobbies[i].used) {
            server->lobbies[i].used = true;
            server->lobbies[i].id = server->next_lobby_id++;
            snprintf(server->lobbies[i].name, sizeof(server->lobbies[i].name), "%s", name[0] ? name : "Lobby");
            trim_token(server->lobbies[i].name);
            game_init(&server->lobbies[i].state);
            created = i;
            break;
        }
    }
    LeaveCriticalSection(&server->lobby_mutex);
    return created;
}

static int add_client_to_lobby(ServerContext *server, int lobby_index, SOCKET socket_fd,
                               const char *player_name, char *message, int message_size) {
    if (lobby_index < 0 || lobby_index >= MAX_LOBBIES || !server->lobbies[lobby_index].used) {
        snprintf(message, message_size, "ERROR Lobby nie istnieje");
        return -1;
    }
    EnterCriticalSection(&server->lobbies[lobby_index].mutex);
    int player_id = game_add_player(&server->lobbies[lobby_index].state, (int)socket_fd,
                                    player_name, message, message_size);
    LeaveCriticalSection(&server->lobbies[lobby_index].mutex);
    return player_id;
}

static void send_to_player(ServerContext *server, int lobby_index, int player_id, const char *line) {
    if (lobby_index < 0 || lobby_index >= MAX_LOBBIES || !server->lobbies[lobby_index].used) return;
    EnterCriticalSection(&server->lobbies[lobby_index].mutex);
    int socket_fd = game_find_socket_by_player(&server->lobbies[lobby_index].state, player_id);
    LeaveCriticalSection(&server->lobbies[lobby_index].mutex);
    if (socket_fd >= 0) net_send_line((SOCKET)socket_fd, line);
}

static void broadcast(ServerContext *server, int lobby_index, const char *line) {
    if (lobby_index < 0 || lobby_index >= MAX_LOBBIES || !server->lobbies[lobby_index].used) return;
    SOCKET sockets[MAX_PLAYERS];
    int count = 0;

    EnterCriticalSection(&server->lobbies[lobby_index].mutex);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (server->lobbies[lobby_index].state.players[i].connected) {
            sockets[count++] = (SOCKET)server->lobbies[lobby_index].state.players[i].socket_fd;
        }
    }
    LeaveCriticalSection(&server->lobbies[lobby_index].mutex);

    for (int i = 0; i < count; i++) net_send_line(sockets[i], line);
}

static void broadcast_state(ServerContext *server, int lobby_index) {
    char state_line[MAX_LINE_LEN * 2];
    if (lobby_index < 0 || lobby_index >= MAX_LOBBIES || !server->lobbies[lobby_index].used) return;
    EnterCriticalSection(&server->lobbies[lobby_index].mutex);
    game_build_state_message(&server->lobbies[lobby_index].state, state_line, sizeof(state_line));
    LeaveCriticalSection(&server->lobbies[lobby_index].mutex);
    broadcast(server, lobby_index, state_line);
}

static GameEvent parse_client_line(const char *line, int player_id, int socket_fd, int lobby_index) {
    GameEvent event;
    memset(&event, 0, sizeof(event));
    event.player_id = player_id;
    event.socket_fd = socket_fd;
    event.lobby_id = lobby_index;
    event.type = EVENT_STATE;

    if (strcmp(line, "START") == 0) event.type = EVENT_START;
    else if (strncmp(line, "MOVE ", 5) == 0) {
        event.type = EVENT_MOVE;
        sscanf(line + 5, "%d %d", &event.arg1, &event.arg2);
    } else if (strcmp(line, "TASK") == 0) event.type = EVENT_TASK;
    else if (strcmp(line, "REPORT") == 0) event.type = EVENT_REPORT;
    else if (strncmp(line, "VOTE ", 5) == 0) {
        event.type = EVENT_VOTE;
        sscanf(line + 5, "%d", &event.arg1);
    } else if (strncmp(line, "KILL ", 5) == 0) {
        event.type = EVENT_KILL;
        sscanf(line + 5, "%d", &event.arg1);
    } else if (strcmp(line, "KILL") == 0) {
        event.type = EVENT_KILL;
        event.arg1 = 0;
    } else if (strcmp(line, "RESET") == 0) event.type = EVENT_RESET;
    else if (strcmp(line, "STATE") == 0) event.type = EVENT_STATE;
    else if (strcmp(line, "QUIT") == 0) event.type = EVENT_QUIT;
    return event;
}

static int handle_lobby_command(ClientContext *client, const char *line) {
    char message[MAX_LINE_LEN];
    char lobby_name[MAX_NAME_LEN + 1] = "Lobby";
    char player_name[MAX_NAME_LEN + 1] = "Player";
    int lobby_id = -1;
    int lobby_index = -1;

    if (strcmp(line, "LIST") == 0) {
        send_lobby_list(client->server, client->socket_fd);
        return 1;
    }
    if (strncmp(line, "CREATE ", 7) == 0) {
        sscanf(line + 7, "%24s %24s", lobby_name, player_name);
        trim_token(lobby_name);
        trim_token(player_name);
        lobby_index = create_lobby(client->server, lobby_name);
        if (lobby_index < 0) {
            net_send_line(client->socket_fd, "ERROR Brak miejsca na nowe lobby");
            return 1;
        }
        int player_id = add_client_to_lobby(client->server, lobby_index, client->socket_fd,
                                            player_name, message, sizeof(message));
        if (player_id < 0) {
            net_send_line(client->socket_fd, message);
            return 1;
        }
        client->lobby_index = lobby_index;
        client->player_id = player_id;
        char welcome[MAX_LINE_LEN * 2];
        snprintf(welcome, sizeof(welcome), "%s lobby=%d name=%s",
                 message, client->server->lobbies[lobby_index].id, client->server->lobbies[lobby_index].name);
        net_send_line(client->socket_fd, welcome);
        broadcast_state(client->server, lobby_index);
        return 1;
    }
    if (strncmp(line, "JOIN ", 5) == 0) {
        if (sscanf(line + 5, "%d %24s", &lobby_id, player_name) != 2) {
            net_send_line(client->socket_fd, "ERROR Uzyj JOIN lobby_id nick");
            return 1;
        }
        trim_token(player_name);
        EnterCriticalSection(&client->server->lobby_mutex);
        lobby_index = find_lobby_index_by_id(client->server, lobby_id);
        LeaveCriticalSection(&client->server->lobby_mutex);
        int player_id = add_client_to_lobby(client->server, lobby_index, client->socket_fd,
                                            player_name, message, sizeof(message));
        if (player_id < 0) {
            net_send_line(client->socket_fd, message);
            return 1;
        }
        client->lobby_index = lobby_index;
        client->player_id = player_id;
        char welcome[MAX_LINE_LEN * 2];
        snprintf(welcome, sizeof(welcome), "%s lobby=%d name=%s",
                 message, client->server->lobbies[lobby_index].id, client->server->lobbies[lobby_index].name);
        net_send_line(client->socket_fd, welcome);
        broadcast_state(client->server, lobby_index);
        return 1;
    }
    return 0;
}

static DWORD WINAPI client_thread(LPVOID parameter) {
    ClientContext *client = (ClientContext *)parameter;
    char line[MAX_LINE_LEN];

    net_send_line(client->socket_fd, "INFO Polaczono. Uzyj LIST, CREATE lobby nick albo JOIN id nick.");
    while (InterlockedCompareExchange(&client->server->running, 1, 1)) {
        int received = net_recv_line(client->socket_fd, line, sizeof(line));
        if (received <= 0) break;

        if (client->player_id <= 0) {
            if (!handle_lobby_command(client, line)) {
                net_send_line(client->socket_fd, "ERROR Najpierw wybierz lobby");
            }
            continue;
        }

        GameEvent event = parse_client_line(line, client->player_id, (int)client->socket_fd, client->lobby_index);
        queue_push(&client->server->queue, &event);
        if (event.type == EVENT_QUIT) break;
    }

    if (client->player_id > 0) {
        GameEvent quit_event;
        memset(&quit_event, 0, sizeof(quit_event));
        quit_event.type = EVENT_QUIT;
        quit_event.player_id = client->player_id;
        quit_event.socket_fd = (int)client->socket_fd;
        quit_event.lobby_id = client->lobby_index;
        queue_push(&client->server->queue, &quit_event);
    }
    net_close_socket(client->socket_fd);
    free(client);
    return 0;
}

static DWORD WINAPI event_loop_thread(LPVOID parameter) {
    ServerContext *server = (ServerContext *)parameter;
    GameEvent event;
    char message[MAX_LINE_LEN];

    while (queue_pop(&server->queue, &event)) {
        int lobby_index = event.lobby_id;
        int direct_player = event.player_id;
        message[0] = '\0';
        if (lobby_index < 0 || lobby_index >= MAX_LOBBIES || !server->lobbies[lobby_index].used) continue;

        EnterCriticalSection(&server->lobbies[lobby_index].mutex);
        GameState *state = &server->lobbies[lobby_index].state;
        switch (event.type) {
            case EVENT_START: game_start(state, event.player_id, message, sizeof(message)); break;
            case EVENT_MOVE: game_move_player(state, event.player_id, event.arg1, event.arg2, message, sizeof(message)); break;
            case EVENT_TASK: game_complete_task(state, event.player_id, message, sizeof(message)); break;
            case EVENT_REPORT: game_report_body(state, event.player_id, message, sizeof(message)); break;
            case EVENT_VOTE: game_vote(state, event.player_id, event.arg1, message, sizeof(message)); break;
            case EVENT_KILL:
                if (event.arg1 == 0) game_kill_nearby(state, event.player_id, message, sizeof(message));
                else game_kill(state, event.player_id, event.arg1, message, sizeof(message));
                break;
            case EVENT_RESET: game_reset_to_lobby(state, event.player_id, message, sizeof(message)); break;
            case EVENT_QUIT: game_remove_player(state, event.player_id, message, sizeof(message)); break;
            case EVENT_STATE: game_build_state_message(state, message, sizeof(message)); break;
            case EVENT_JOIN: break;
        }
        LeaveCriticalSection(&server->lobbies[lobby_index].mutex);

        if (message[0]) {
            if (strncmp(message, "ERROR", 5) == 0 || event.type == EVENT_STATE) {
                send_to_player(server, lobby_index, direct_player, message);
            } else {
                broadcast(server, lobby_index, message);
            }
        }
        if (event.type != EVENT_STATE) broadcast_state(server, lobby_index);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *port = argc > 1 ? argv[1] : DEFAULT_PORT;
    ServerContext server;
    memset(&server, 0, sizeof(server));
    InterlockedExchange(&server.running, 1);
    server.next_lobby_id = 1;

    if (!net_startup()) {
        fprintf(stderr, "Nie mozna uruchomic Winsock\n");
        return 1;
    }

    queue_init(&server.queue);
    InitializeCriticalSection(&server.lobby_mutex);
    for (int i = 0; i < MAX_LOBBIES; i++) {
        InitializeCriticalSection(&server.lobbies[i].mutex);
    }

    server.server_socket = net_create_server_socket(port);
    if (server.server_socket == INVALID_SOCKET) {
        fprintf(stderr, "Nie mozna uruchomic serwera na porcie %s\n", port);
        return 1;
    }

    CreateThread(NULL, 0, event_loop_thread, &server, 0, NULL);
    printf("Serwer dziala na porcie %s i obsluguje do %d lobby\n", port, MAX_LOBBIES);

    while (InterlockedCompareExchange(&server.running, 1, 1)) {
        SOCKET client_socket = accept(server.server_socket, NULL, NULL);
        if (client_socket == INVALID_SOCKET) break;
        ClientContext *client = (ClientContext *)calloc(1, sizeof(ClientContext));
        if (!client) {
            net_close_socket(client_socket);
            continue;
        }
        client->server = &server;
        client->socket_fd = client_socket;
        client->player_id = -1;
        client->lobby_index = -1;
        CreateThread(NULL, 0, client_thread, client, 0, NULL);
    }

    queue_close(&server.queue);
    net_close_socket(server.server_socket);
    queue_destroy(&server.queue);
    for (int i = 0; i < MAX_LOBBIES; i++) DeleteCriticalSection(&server.lobbies[i].mutex);
    DeleteCriticalSection(&server.lobby_mutex);
    net_cleanup();
    return 0;
}
