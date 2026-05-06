#define WIN32_LEAN_AND_MEAN
#include "net.h"
#include <conio.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "config.h"

typedef struct ViewPlayer {
    int id;
    int x;
    int y;
    int alive;
    char name[MAX_NAME_LEN + 1];
    char role[16];
} ViewPlayer;

typedef struct ViewState {
    int version;
    int width;
    int height;
    int task_done;
    int task_goal;
    int task_spots[TASK_SPOT_COUNT][2];
    int task_spot_count;
    char phase[24];
    char winner[64];
    ViewPlayer players[MAX_PLAYERS];
    int player_count;
    char last_message[MAX_LINE_LEN];
} ViewState;

typedef struct ClientRuntime {
    SOCKET socket_fd;
    volatile LONG running;
    CRITICAL_SECTION view_mutex;
    ViewState view;
} ClientRuntime;

static void init_view(ViewState *view) {
    memset(view, 0, sizeof(*view));
    view->width = MAP_WIDTH;
    view->height = MAP_HEIGHT;
    view->task_goal = TASKS_TO_WIN;
    snprintf(view->phase, sizeof(view->phase), "LOBBY");
    snprintf(view->winner, sizeof(view->winner), "-");
}

static const char *value_after(const char *line, const char *key) {
    const char *found = strstr(line, key);
    return found ? found + strlen(key) : NULL;
}

static void copy_token(char *dest, int dest_size, const char *source) {
    int i = 0;
    while (source && source[i] && source[i] != ' ' && i < dest_size - 1) {
        dest[i] = source[i];
        i++;
    }
    dest[i] = '\0';
}

static void copy_text(char *dest, int dest_size, const char *source) {
    if (dest_size <= 0) {
        return;
    }
    strncpy(dest, source ? source : "", dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static void parse_task_spots(ViewState *view, const char *spots) {
    char local[MAX_LINE_LEN];
    char *context = NULL;
    char *token = NULL;

    view->task_spot_count = 0;
    copy_token(local, sizeof(local), spots);
    token = strtok_s(local, "|", &context);
    while (token && view->task_spot_count < TASK_SPOT_COUNT) {
        int x = 0;
        int y = 0;
        if (sscanf(token, "%d,%d", &x, &y) == 2) {
            view->task_spots[view->task_spot_count][0] = x;
            view->task_spots[view->task_spot_count][1] = y;
            view->task_spot_count++;
        }
        token = strtok_s(NULL, "|", &context);
    }
}

static void parse_players(ViewState *view, const char *players) {
    char local[MAX_LINE_LEN * 2];
    char *context = NULL;
    char *token = NULL;

    view->player_count = 0;
    snprintf(local, sizeof(local), "%s", players);
    token = strtok_s(local, ";", &context);
    while (token && view->player_count < MAX_PLAYERS) {
        ViewPlayer player;
        char life[16];
        memset(&player, 0, sizeof(player));
        if (sscanf(token, "%d:%24[^:]:%15[^:]:%15[^:]:", &player.id, player.name, player.role, life) == 4) {
            char *pos = strrchr(token, '(');
            if (pos && sscanf(pos, "(%d,%d)", &player.x, &player.y) == 2) {
                player.alive = strcmp(life, "alive") == 0;
                view->players[view->player_count++] = player;
            }
        }
        token = strtok_s(NULL, ";", &context);
    }
}

static void parse_state_line(ViewState *view, const char *line) {
    const char *value = NULL;

    value = value_after(line, "version=");
    if (value) {
        sscanf(value, "%d", &view->version);
    }
    value = value_after(line, "phase=");
    if (value) {
        copy_token(view->phase, sizeof(view->phase), value);
    }
    value = value_after(line, "map=");
    if (value) {
        sscanf(value, "%dx%d", &view->width, &view->height);
    }
    value = value_after(line, "tasks=");
    if (value) {
        sscanf(value, "%d/%d", &view->task_done, &view->task_goal);
    }
    value = value_after(line, "winner=");
    if (value) {
        copy_token(view->winner, sizeof(view->winner), value);
    }
    value = value_after(line, "taskspots=");
    if (value) {
        parse_task_spots(view, value);
    }
    value = value_after(line, "players=");
    if (value) {
        parse_players(view, value);
    }
}

static DWORD WINAPI receive_thread(LPVOID parameter) {
    ClientRuntime *runtime = (ClientRuntime *)parameter;
    char line[MAX_LINE_LEN * 2];

    while (InterlockedCompareExchange(&runtime->running, 1, 1)) {
        int received = net_recv_line(runtime->socket_fd, line, sizeof(line));
        if (received <= 0) {
            EnterCriticalSection(&runtime->view_mutex);
            snprintf(runtime->view.last_message, sizeof(runtime->view.last_message), "Rozlaczono z serwerem");
            LeaveCriticalSection(&runtime->view_mutex);
            InterlockedExchange(&runtime->running, 0);
            break;
        }

        EnterCriticalSection(&runtime->view_mutex);
        if (strncmp(line, "STATE ", 6) == 0) {
            parse_state_line(&runtime->view, line);
        } else {
            copy_text(runtime->view.last_message, sizeof(runtime->view.last_message), line);
        }
        LeaveCriticalSection(&runtime->view_mutex);
    }
    return 0;
}

static int has_task_spot(const ViewState *view, int x, int y) {
    for (int i = 0; i < view->task_spot_count; i++) {
        if (view->task_spots[i][0] == x && view->task_spots[i][1] == y) {
            return 1;
        }
    }
    return 0;
}

static char player_symbol(const ViewPlayer *player) {
    if (!player->alive) {
        return 'x';
    }
    if (strcmp(player->role, "impostor") == 0) {
        return 'I';
    }
    if (strcmp(player->role, "crewmate") == 0) {
        return 'C';
    }
    return (char)('0' + (player->id % 10));
}

static void render(const ViewState *view) {
    char grid[MAP_HEIGHT][MAP_WIDTH];
    system("cls");

    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            grid[y][x] = has_task_spot(view, x, y) ? 'T' : '.';
        }
    }
    for (int i = 0; i < view->player_count; i++) {
        int x = view->players[i].x;
        int y = view->players[i].y;
        if (x >= 0 && x < MAP_WIDTH && y >= 0 && y < MAP_HEIGHT) {
            grid[y][x] = player_symbol(&view->players[i]);
        }
    }

    printf("AmongUs terminal realtime | faza: %s | zadania: %d/%d | wersja: %d\n",
           view->phase, view->task_done, view->task_goal, view->version);
    if (strcmp(view->winner, "-") != 0) {
        printf("Zwyciezca: %s\n", view->winner);
    }
    printf("+");
    for (int x = 0; x < MAP_WIDTH; x++) printf("-");
    printf("+\n");
    for (int y = 0; y < MAP_HEIGHT; y++) {
        printf("|");
        for (int x = 0; x < MAP_WIDTH; x++) {
            printf("%c", grid[y][x]);
        }
        printf("|\n");
    }
    printf("+");
    for (int x = 0; x < MAP_WIDTH; x++) printf("-");
    printf("+\n");

    printf("Gracze: ");
    for (int i = 0; i < view->player_count; i++) {
        printf("%d:%s[%s,%s](%d,%d) ",
               view->players[i].id,
               view->players[i].name,
               view->players[i].role,
               view->players[i].alive ? "alive" : "dead",
               view->players[i].x,
               view->players[i].y);
    }
    printf("\n");
    printf("Sterowanie: W/A/S/D ruch | E zadanie | R report | X start | V glos | K kill | Q wyjscie\n");
    printf("Ostatnio: %s\n", view->last_message[0] ? view->last_message : "-");
}

static void send_vote(ClientRuntime *runtime) {
    char input[32];
    printf("\nID gracza do glosu, 0 = skip: ");
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin)) {
        char command[64];
        input[strcspn(input, "\r\n")] = '\0';
        snprintf(command, sizeof(command), "VOTE %s", input);
        net_send_line(runtime->socket_fd, command);
    }
}

static void send_kill(ClientRuntime *runtime) {
    char input[32];
    printf("\nID gracza do zabicia: ");
    fflush(stdout);
    if (fgets(input, sizeof(input), stdin)) {
        char command[64];
        input[strcspn(input, "\r\n")] = '\0';
        snprintf(command, sizeof(command), "KILL %s", input);
        net_send_line(runtime->socket_fd, command);
    }
}

static void handle_key(ClientRuntime *runtime, int key) {
    key = tolower(key);
    if (key == 'w') {
        net_send_line(runtime->socket_fd, "MOVE 0 -1");
    } else if (key == 's') {
        net_send_line(runtime->socket_fd, "MOVE 0 1");
    } else if (key == 'a') {
        net_send_line(runtime->socket_fd, "MOVE -1 0");
    } else if (key == 'd') {
        net_send_line(runtime->socket_fd, "MOVE 1 0");
    } else if (key == 'e') {
        net_send_line(runtime->socket_fd, "TASK");
    } else if (key == 'r') {
        net_send_line(runtime->socket_fd, "REPORT");
    } else if (key == 'x') {
        net_send_line(runtime->socket_fd, "START");
    } else if (key == 'v') {
        send_vote(runtime);
    } else if (key == 'k') {
        send_kill(runtime);
    } else if (key == 'q') {
        net_send_line(runtime->socket_fd, "QUIT");
        InterlockedExchange(&runtime->running, 0);
    }
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : DEFAULT_PORT;
    char name[MAX_NAME_LEN + 1];
    char command[MAX_LINE_LEN];

    if (!net_startup()) {
        fprintf(stderr, "Nie mozna uruchomic Winsock\n");
        return 1;
    }

    SOCKET socket_fd = net_connect_to_server(host, port);
    if (socket_fd == INVALID_SOCKET) {
        fprintf(stderr, "Nie mozna polaczyc z %s:%s\n", host, port);
        net_cleanup();
        return 1;
    }

    printf("Podaj nazwe gracza: ");
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin)) {
        net_close_socket(socket_fd);
        net_cleanup();
        return 1;
    }
    name[strcspn(name, "\r\n")] = '\0';
    snprintf(command, sizeof(command), "JOIN %s", name[0] ? name : "Player");
    net_send_line(socket_fd, command);

    ClientRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.socket_fd = socket_fd;
    InterlockedExchange(&runtime.running, 1);
    InitializeCriticalSection(&runtime.view_mutex);
    init_view(&runtime.view);

    CreateThread(NULL, 0, receive_thread, &runtime, 0, NULL);
    net_send_line(socket_fd, "STATE");

    while (InterlockedCompareExchange(&runtime.running, 1, 1)) {
        ViewState snapshot;
        EnterCriticalSection(&runtime.view_mutex);
        snapshot = runtime.view;
        LeaveCriticalSection(&runtime.view_mutex);
        render(&snapshot);

        for (int i = 0; i < 10; i++) {
            if (_kbhit()) {
                int key = _getch();
                handle_key(&runtime, key);
                break;
            }
            Sleep(20);
        }
    }

    net_close_socket(socket_fd);
    DeleteCriticalSection(&runtime.view_mutex);
    net_cleanup();
    return 0;
}
