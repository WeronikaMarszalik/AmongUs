#define WIN32_LEAN_AND_MEAN
#include "net.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "config.h"

#define WINDOW_WIDTH 960
#define WINDOW_HEIGHT 640
#define TILE_SIZE 96
#define MOVE_INTERVAL_MS 150
#define CREWMATE_VISION_RADIUS 155
#define TASK_PROGRESS_MS 2200
#define TASK_CODE_SHOW_MS 1400

typedef enum TaskMode {
    TASK_UI_NONE,
    TASK_UI_PROGRESS,
    TASK_UI_SHOW_CODE,
    TASK_UI_INPUT_CODE,
    TASK_UI_DONE
} TaskMode;

typedef enum TaskKind {
    TASK_KIND_NONE,
    TASK_KIND_PROGRESS,
    TASK_KIND_CODE,
    TASK_KIND_CARD
} TaskKind;

typedef struct ViewPlayer {
    int id;
    int x;
    int y;
    int alive;
    char name[MAX_NAME_LEN + 1];
    char role[16];
} ViewPlayer;

typedef struct ViewBody {
    int x;
    int y;
    int victim_id;
} ViewBody;

typedef struct ViewState {
    int version;
    int task_done;
    int task_goal;
    int task_spots[TASK_SPOT_COUNT][2];
    char task_types[TASK_SPOT_COUNT];
    int task_spot_count;
    int local_player_id;
    char phase[24];
    char winner[64];
    char vote_result[128];
    int vote_result_version;
    ViewPlayer players[MAX_PLAYERS];
    int player_count;
    ViewBody bodies[MAX_PLAYERS];
    int body_count;
    char last_message[MAX_LINE_LEN];
} ViewState;

typedef struct ClientRuntime {
    SOCKET socket_fd;
    volatile LONG running;
    CRITICAL_SECTION view_mutex;
    ViewState view;
} ClientRuntime;

static int parse_welcome_player_id(const char *line) {
    int id = -1;
    if (sscanf(line, "WELCOME %d", &id) == 1) return id;
    return -1;
}

typedef struct Assets {
    TTF_Font *font;
    TTF_Font *small_font;
    TTF_Font *large_font;
} Assets;

typedef struct TaskUi {
    TaskMode mode;
    TaskKind kind;
    int task_index;
    Uint32 started_at;
    int series_done;
    int card_steps;
    char code[5];
    char input[5];
    char status[96];
} TaskUi;

typedef struct VoteUi {
    char input[8];
    int vote_sent;
    char message[96];
} VoteUi;

static void copy_text(char *dest, int dest_size, const char *source) {
    if (dest_size <= 0) return;
    strncpy(dest, source ? source : "", dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static void copy_token(char *dest, int dest_size, const char *source) {
    int i = 0;
    while (source && source[i] && source[i] != ' ' && i < dest_size - 1) {
        dest[i] = source[i];
        i++;
    }
    dest[i] = '\0';
}

static const char *value_after(const char *line, const char *key) {
    const char *found = strstr(line, key);
    return found ? found + strlen(key) : NULL;
}

static void init_view(ViewState *view) {
    memset(view, 0, sizeof(*view));
    view->task_goal = TASKS_TO_WIN;
    view->local_player_id = -1;
    copy_text(view->phase, sizeof(view->phase), "LOBBY");
    copy_text(view->winner, sizeof(view->winner), "-");
    copy_text(view->last_message, sizeof(view->last_message), "Dolaczanie do lobby...");
}

static void remember_vote_result(ViewState *view, const char *line) {
    if (strstr(line, "zostaje wyrzucony") || strstr(line, "Glosowanie bez wyrzucenia")) {
        copy_text(view->vote_result, sizeof(view->vote_result), line);
        view->vote_result_version++;
    }
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
        char type = 'P';
        if (sscanf(token, "%d,%d,%c", &x, &y, &type) >= 2) {
            view->task_spots[view->task_spot_count][0] = x;
            view->task_spots[view->task_spot_count][1] = y;
            view->task_types[view->task_spot_count] = type;
            view->task_spot_count++;
        }
        token = strtok_s(NULL, "|", &context);
    }
}

static void parse_bodies(ViewState *view, const char *bodies) {
    char local[MAX_LINE_LEN];
    char *context = NULL;
    char *token = NULL;

    view->body_count = 0;
    copy_token(local, sizeof(local), bodies);
    if (local[0] == '\0') return;
    token = strtok_s(local, "|", &context);
    while (token && view->body_count < MAX_PLAYERS) {
        ViewBody body;
        memset(&body, 0, sizeof(body));
        if (sscanf(token, "%d,%d,%d", &body.x, &body.y, &body.victim_id) == 3) {
            view->bodies[view->body_count++] = body;
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
    const char *value = value_after(line, "version=");
    if (value) sscanf(value, "%d", &view->version);
    value = value_after(line, "phase=");
    if (value) copy_token(view->phase, sizeof(view->phase), value);
    value = value_after(line, "tasks=");
    if (value) sscanf(value, "%d/%d", &view->task_done, &view->task_goal);
    value = value_after(line, "winner=");
    if (value) copy_token(view->winner, sizeof(view->winner), value);
    value = value_after(line, "taskspots=");
    if (value) parse_task_spots(view, value);
    value = value_after(line, "players=");
    if (value) parse_players(view, value);
    value = value_after(line, "bodies=");
    if (value) parse_bodies(view, value);
}

static void parse_welcome(ViewState *view, const char *line) {
    int id = parse_welcome_player_id(line);
    if (id > 0) view->local_player_id = id;
}

static int read_until_prefix(SOCKET socket_fd, const char *prefix, char *out, int out_size) {
    char line[MAX_LINE_LEN * 2];
    for (int i = 0; i < 20; i++) {
        int received = net_recv_line(socket_fd, line, sizeof(line));
        if (received <= 0) return 0;
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            copy_text(out, out_size, line);
            return 1;
        }
        if (strncmp(line, "ERROR", 5) == 0) {
            copy_text(out, out_size, line);
            return 0;
        }
    }
    return 0;
}

static void print_lobby_list(const char *line) {
    const char *payload = line + strlen("LOBBIES ");
    char local[MAX_LINE_LEN * 2];
    char *context = NULL;
    char *token = NULL;
    int any = 0;
    copy_text(local, sizeof(local), payload);
    token = strtok_s(local, "|", &context);
    while (token) {
        int id = 0;
        char name[MAX_NAME_LEN + 1] = "";
        char count[32] = "";
        char phase[24] = "";
        if (sscanf(token, "%d:%24[^:]:%31[^:]:%23s", &id, name, count, phase) == 4) {
            printf("  %d) %s  gracze %s  faza %s\n", id, name, count, phase);
            any = 1;
        }
        token = strtok_s(NULL, "|", &context);
    }
    if (!any) printf("  Brak lobby na tym serwerze.\n");
}

static DWORD WINAPI receive_thread(LPVOID parameter) {
    ClientRuntime *runtime = (ClientRuntime *)parameter;
    char line[MAX_LINE_LEN * 2];

    while (InterlockedCompareExchange(&runtime->running, 1, 1)) {
        int received = net_recv_line(runtime->socket_fd, line, sizeof(line));
        EnterCriticalSection(&runtime->view_mutex);
        if (received <= 0) {
            copy_text(runtime->view.last_message, sizeof(runtime->view.last_message), "Rozlaczono z serwerem");
            InterlockedExchange(&runtime->running, 0);
            LeaveCriticalSection(&runtime->view_mutex);
            break;
        }
        if (strncmp(line, "STATE ", 6) == 0) {
            parse_state_line(&runtime->view, line);
        } else {
            if (strncmp(line, "WELCOME ", 8) == 0) {
                parse_welcome(&runtime->view, line);
            }
            remember_vote_result(&runtime->view, line);
            copy_text(runtime->view.last_message, sizeof(runtime->view.last_message), line);
        }
        LeaveCriticalSection(&runtime->view_mutex);
    }
    return 0;
}

static TTF_Font *open_font(int size) {
    const char *paths[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/msys64/ucrt64/share/fonts/TTF/DejaVuSans.ttf"
    };
    for (int i = 0; i < 3; i++) {
        TTF_Font *font = TTF_OpenFont(paths[i], size);
        if (font) return font;
    }
    return NULL;
}

static int load_assets(Assets *assets) {
    assets->font = open_font(22);
    assets->small_font = open_font(16);
    assets->large_font = open_font(42);
    return assets->font && assets->small_font && assets->large_font;
}

static void destroy_assets(Assets *assets) {
    if (assets->font) TTF_CloseFont(assets->font);
    if (assets->small_font) TTF_CloseFont(assets->small_font);
    if (assets->large_font) TTF_CloseFont(assets->large_font);
}

static void set_color(SDL_Renderer *renderer, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

static void fill_rect(SDL_Renderer *renderer, int x, int y, int w, int h, SDL_Color color) {
    if (w <= 0 || h <= 0) return;
    SDL_Rect rect = {x, y, w, h};
    set_color(renderer, color);
    SDL_RenderFillRect(renderer, &rect);
}

static void draw_text(SDL_Renderer *renderer, TTF_Font *font, const char *text, int x, int y, SDL_Color color) {
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst = {x, y, surface->w, surface->h};
        SDL_RenderCopy(renderer, texture, NULL, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_FreeSurface(surface);
}

static SDL_Color player_color(const ViewPlayer *player) {
    static SDL_Color colors[] = {
        {215, 55, 70, 255}, {45, 130, 235, 255}, {60, 190, 115, 255},
        {238, 190, 70, 255}, {196, 88, 220, 255}, {242, 126, 55, 255},
        {65, 205, 205, 255}, {240, 94, 170, 255}, {170, 178, 188, 255},
        {118, 82, 55, 255}
    };
    if (!player->alive) return (SDL_Color){92, 96, 106, 255};
    return colors[player->id % 10];
}

static const ViewPlayer *local_player(const ViewState *view) {
    for (int i = 0; i < view->player_count; i++) {
        if (view->players[i].id == view->local_player_id) {
            return &view->players[i];
        }
    }
    return view->player_count > 0 ? &view->players[0] : NULL;
}

static TaskKind task_kind_at(const ViewState *view, int x, int y) {
    for (int i = 0; i < view->task_spot_count; i++) {
        if (view->task_spots[i][0] == x && view->task_spots[i][1] == y) {
            if (view->task_types[i] == 'C') return TASK_KIND_CODE;
            if (view->task_types[i] == 'S') return TASK_KIND_CARD;
            return TASK_KIND_PROGRESS;
        }
    }
    return TASK_KIND_NONE;
}

static int task_index_at(const ViewState *view, int x, int y) {
    for (int i = 0; i < view->task_spot_count; i++) {
        if (view->task_spots[i][0] == x && view->task_spots[i][1] == y) return i;
    }
    return -1;
}

static int body_near_local_player(const ViewState *view) {
    const ViewPlayer *me = local_player(view);
    if (!me) return 0;
    for (int i = 0; i < view->body_count; i++) {
        int dx = view->bodies[i].x - me->x;
        int dy = view->bodies[i].y - me->y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy <= 1) return 1;
    }
    return 0;
}

static const ViewPlayer *find_view_player(const ViewState *view, int player_id) {
    for (int i = 0; i < view->player_count; i++) {
        if (view->players[i].id == player_id) return &view->players[i];
    }
    return NULL;
}

static void generate_task_code(TaskUi *task) {
    for (int i = 0; i < 4; i++) {
        task->code[i] = (char)('0' + rand() % 10);
    }
    task->code[4] = '\0';
    task->input[0] = '\0';
}

static void start_task_ui(TaskUi *task, TaskKind kind, int task_index) {
    memset(task, 0, sizeof(*task));
    task->kind = kind;
    task->task_index = task_index;
    task->mode = kind == TASK_KIND_CODE ? TASK_UI_SHOW_CODE : TASK_UI_PROGRESS;
    task->started_at = SDL_GetTicks();
    if (kind == TASK_KIND_CODE) {
        generate_task_code(task);
        copy_text(task->status, sizeof(task->status), "Zapamietaj kod");
        SDL_StartTextInput();
    } else if (kind == TASK_KIND_CARD) {
        copy_text(task->status, sizeof(task->status), "Swipe card: naciskaj strzalke w prawo");
    } else {
        copy_text(task->status, sizeof(task->status), "Downloading data...");
    }
}

static void advance_task_ui(TaskUi *task, ClientRuntime *runtime, unsigned int *local_completed_tasks) {
    Uint32 now = SDL_GetTicks();
    if (task->mode == TASK_UI_PROGRESS && task->kind == TASK_KIND_PROGRESS &&
        now - task->started_at >= TASK_PROGRESS_MS) {
        task->mode = TASK_UI_DONE;
        task->started_at = now;
        copy_text(task->status, sizeof(task->status), "Progress task complete");
    } else if (task->mode == TASK_UI_SHOW_CODE && now - task->started_at >= TASK_CODE_SHOW_MS) {
        task->mode = TASK_UI_INPUT_CODE;
        task->input[0] = '\0';
        copy_text(task->status, sizeof(task->status), "Wpisz kod");
    } else if (task->mode == TASK_UI_DONE && now - task->started_at >= 550) {
        net_send_line(runtime->socket_fd, "TASK");
        if (task->task_index >= 0) {
            *local_completed_tasks |= (1u << task->task_index);
        }
        task->mode = TASK_UI_NONE;
        SDL_StopTextInput();
    }
}

static void draw_bean(SDL_Renderer *renderer, TTF_Font *font, const ViewPlayer *player, int x, int y, int local) {
    SDL_Color color = player_color(player);
    SDL_Rect body = {x - 18, y - 24, 36, 48};
    SDL_Rect visor = {x - 2, y - 16, 18, 12};
    SDL_Rect shadow = {x - 19, y + 18, 38, 8};

    fill_rect(renderer, shadow.x, shadow.y, shadow.w, shadow.h, (SDL_Color){0, 0, 0, 80});
    fill_rect(renderer, body.x, body.y, body.w, body.h, color);
    fill_rect(renderer, visor.x, visor.y, visor.w, visor.h, (SDL_Color){180, 230, 245, 255});
    if (local) {
        SDL_Rect outline = {body.x - 3, body.y - 3, body.w + 6, body.h + 6};
        set_color(renderer, (SDL_Color){255, 255, 255, 255});
        SDL_RenderDrawRect(renderer, &outline);
    }
    if (!player->alive) {
        set_color(renderer, (SDL_Color){240, 60, 70, 255});
        SDL_RenderDrawLine(renderer, x - 24, y - 24, x + 24, y + 24);
        SDL_RenderDrawLine(renderer, x + 24, y - 24, x - 24, y + 24);
    }
    draw_text(renderer, font, player->name, x - 32, y - 52, (SDL_Color){235, 240, 245, 255});
}

static void draw_button(SDL_Renderer *renderer, TTF_Font *font, int x, int y, int w, int h,
                        const char *label, SDL_Color color) {
    fill_rect(renderer, x, y, w, h, color);
    SDL_Rect outline = {x, y, w, h};
    set_color(renderer, (SDL_Color){235, 240, 245, 255});
    SDL_RenderDrawRect(renderer, &outline);
    draw_text(renderer, font, label, x + 14, y + 12, (SDL_Color){255, 255, 255, 255});
}

static void draw_task_bar(SDL_Renderer *renderer, const ViewState *view) {
    int x = 24;
    int y = 18;
    int w = 390;
    int h = 22;
    int fill = view->task_goal > 0 ? (w * view->task_done) / view->task_goal : 0;
    fill_rect(renderer, x, y, w, h, (SDL_Color){28, 35, 40, 255});
    fill_rect(renderer, x, y, fill, h, (SDL_Color){61, 213, 74, 255});
    SDL_Rect outline = {x, y, w, h};
    set_color(renderer, (SDL_Color){230, 235, 240, 255});
    SDL_RenderDrawRect(renderer, &outline);
}

static void render_lobby(SDL_Renderer *renderer, Assets *assets, const ViewState *view) {
    set_color(renderer, (SDL_Color){18, 22, 30, 255});
    SDL_RenderClear(renderer);

    fill_rect(renderer, 80, 100, 620, 430, (SDL_Color){68, 83, 94, 255});
    fill_rect(renderer, 120, 140, 540, 350, (SDL_Color){91, 109, 122, 255});
    fill_rect(renderer, 360, 250, 110, 70, (SDL_Color){55, 65, 75, 255});
    fill_rect(renderer, 388, 228, 54, 24, (SDL_Color){32, 39, 48, 255});

    draw_text(renderer, assets->large_font, "LOBBY", 735, 80, (SDL_Color){255, 255, 255, 255});
    char count[64];
    snprintf(count, sizeof(count), "Players: %d/%d", view->player_count, MAX_PLAYERS);
    draw_text(renderer, assets->font, count, 735, 145, (SDL_Color){235, 240, 245, 255});
    draw_text(renderer, assets->small_font, "Private room: LOCAL", 735, 180, (SDL_Color){190, 205, 215, 255});
    draw_text(renderer, assets->small_font, "Map: The Skeld mini", 735, 205, (SDL_Color){190, 205, 215, 255});
    draw_text(renderer, assets->small_font, "Impostors: 1", 735, 230, (SDL_Color){190, 205, 215, 255});
    draw_text(renderer, assets->small_font, "Move with WASD", 735, 285, (SDL_Color){255, 230, 120, 255});
    draw_text(renderer, assets->small_font, "Press X to start", 735, 310, (SDL_Color){255, 230, 120, 255});
    draw_button(renderer, assets->font, 735, 365, 170, 56, "START  X", (SDL_Color){42, 154, 86, 255});

    for (int i = 0; i < view->player_count; i++) {
        int px = 135 + view->players[i].x * 64;
        int py = 160 + view->players[i].y * 64;
        draw_bean(renderer, assets->small_font, &view->players[i], px, py, view->players[i].id == view->local_player_id);
    }
    draw_text(renderer, assets->small_font, view->last_message, 90, 555, (SDL_Color){235, 240, 245, 255});
}

static void draw_room(SDL_Renderer *renderer, int world_x, int world_y, int w, int h, int camera_x, int camera_y,
                      SDL_Color color) {
    int sx = world_x - camera_x;
    int sy = world_y - camera_y;
    fill_rect(renderer, sx, sy, w, h, color);
    SDL_Rect outline = {sx, sy, w, h};
    set_color(renderer, (SDL_Color){82, 92, 96, 255});
    SDL_RenderDrawRect(renderer, &outline);
}

static void draw_walls(SDL_Renderer *renderer, int camera_x, int camera_y) {
    for (int i = 0; i < WALL_COUNT; i++) {
        int sx = WALLS[i][0] * TILE_SIZE - camera_x;
        int sy = WALLS[i][1] * TILE_SIZE - camera_y;
        int sw = WALLS[i][2] * TILE_SIZE;
        int sh = WALLS[i][3] * TILE_SIZE;
        fill_rect(renderer, sx, sy, sw, sh, (SDL_Color){24, 30, 34, 255});
        SDL_Rect outline = {sx, sy, sw, sh};
        set_color(renderer, (SDL_Color){96, 107, 116, 255});
        SDL_RenderDrawRect(renderer, &outline);
    }
}

static void apply_crewmate_vision(SDL_Renderer *renderer, const ViewState *view, int camera_x, int camera_y) {
    const ViewPlayer *me = local_player(view);
    if (!me || strcmp(me->role, "crewmate") != 0) return;

    int cx = me->x * TILE_SIZE - camera_x + TILE_SIZE / 2;
    int cy = me->y * TILE_SIZE - camera_y + TILE_SIZE / 2;
    int radius = CREWMATE_VISION_RADIUS;

    const int block = 16;
    int radius2 = radius * radius;
    for (int y = 0; y < WINDOW_HEIGHT; y += block) {
        for (int x = 0; x < WINDOW_WIDTH; x += block) {
            int bx = x + block / 2;
            int by = y + block / 2;
            int dx = bx - cx;
            int dy = by - cy;
            int dist2 = dx * dx + dy * dy;
            if (dist2 > radius2) {
                fill_rect(renderer, x, y, block, block, (SDL_Color){0, 0, 0, 230});
            } else if (dist2 > radius2 - 4200) {
                fill_rect(renderer, x, y, block, block, (SDL_Color){0, 0, 0, 95});
            }
        }
    }
}

static void render_game(SDL_Renderer *renderer, Assets *assets, const ViewState *view, const VoteUi *vote_ui) {
    const ViewPlayer *me = local_player(view);
    int camera_x = me ? me->x * TILE_SIZE - WINDOW_WIDTH / 2 : 0;
    int camera_y = me ? me->y * TILE_SIZE - WINDOW_HEIGHT / 2 : 0;
    int max_x = MAP_WIDTH * TILE_SIZE - WINDOW_WIDTH;
    int max_y = MAP_HEIGHT * TILE_SIZE - WINDOW_HEIGHT;
    if (camera_x < 0) camera_x = 0;
    if (camera_y < 0) camera_y = 0;
    if (camera_x > max_x) camera_x = max_x > 0 ? max_x : 0;
    if (camera_y > max_y) camera_y = max_y > 0 ? max_y : 0;

    set_color(renderer, (SDL_Color){8, 10, 14, 255});
    SDL_RenderClear(renderer);

    for (int y = 0; y < MAP_HEIGHT; y++) {
        for (int x = 0; x < MAP_WIDTH; x++) {
            SDL_Color tile = ((x + y) % 2 == 0) ? (SDL_Color){45, 52, 58, 255} : (SDL_Color){50, 58, 64, 255};
            fill_rect(renderer, x * TILE_SIZE - camera_x, y * TILE_SIZE - camera_y, TILE_SIZE, TILE_SIZE, tile);
        }
    }

    draw_room(renderer, 1 * TILE_SIZE, 1 * TILE_SIZE, 4 * TILE_SIZE, 4 * TILE_SIZE, camera_x, camera_y, (SDL_Color){85, 78, 116, 255});
    draw_room(renderer, 5 * TILE_SIZE, 1 * TILE_SIZE, 5 * TILE_SIZE, 4 * TILE_SIZE, camera_x, camera_y, (SDL_Color){76, 95, 110, 255});
    draw_room(renderer, 10 * TILE_SIZE, 1 * TILE_SIZE, 5 * TILE_SIZE, 4 * TILE_SIZE, camera_x, camera_y, (SDL_Color){82, 94, 74, 255});
    draw_room(renderer, 15 * TILE_SIZE, 1 * TILE_SIZE, 4 * TILE_SIZE, 4 * TILE_SIZE, camera_x, camera_y, (SDL_Color){74, 92, 72, 255});
    draw_room(renderer, 2 * TILE_SIZE, 5 * TILE_SIZE, 7 * TILE_SIZE, 5 * TILE_SIZE, camera_x, camera_y, (SDL_Color){67, 82, 84, 255});
    draw_room(renderer, 10 * TILE_SIZE, 5 * TILE_SIZE, 8 * TILE_SIZE, 5 * TILE_SIZE, camera_x, camera_y, (SDL_Color){64, 76, 92, 255});
    draw_room(renderer, 1 * TILE_SIZE, 10 * TILE_SIZE, 18 * TILE_SIZE, 3 * TILE_SIZE, camera_x, camera_y, (SDL_Color){76, 70, 84, 255});
    draw_walls(renderer, camera_x, camera_y);

    for (int i = 0; i < view->task_spot_count; i++) {
        int sx = view->task_spots[i][0] * TILE_SIZE - camera_x + 18;
        int sy = view->task_spots[i][1] * TILE_SIZE - camera_y + 18;
        SDL_Color terminal_color = view->task_types[i] == 'C'
            ? (SDL_Color){90, 170, 245, 255}
            : view->task_types[i] == 'S'
                ? (SDL_Color){218, 90, 180, 255}
                : (SDL_Color){238, 190, 66, 255};
        fill_rect(renderer, sx, sy, 42, 42, terminal_color);
        draw_text(renderer, assets->small_font,
                  view->task_types[i] == 'C' ? "CODE" : view->task_types[i] == 'S' ? "CARD" : "LOAD",
                  sx - 4, sy + 48, (SDL_Color){255, 245, 185, 255});
    }

    for (int i = 0; i < view->body_count; i++) {
        int sx = view->bodies[i].x * TILE_SIZE - camera_x + TILE_SIZE / 2;
        int sy = view->bodies[i].y * TILE_SIZE - camera_y + TILE_SIZE / 2;
        fill_rect(renderer, sx - 22, sy - 12, 44, 24, (SDL_Color){150, 28, 40, 255});
        fill_rect(renderer, sx - 8, sy - 26, 16, 16, (SDL_Color){235, 235, 220, 255});
    }

    for (int i = 0; i < view->player_count; i++) {
        int px = view->players[i].x * TILE_SIZE - camera_x + TILE_SIZE / 2;
        int py = view->players[i].y * TILE_SIZE - camera_y + TILE_SIZE / 2;
        draw_bean(renderer, assets->small_font, &view->players[i], px, py, view->players[i].id == view->local_player_id);
    }

    apply_crewmate_vision(renderer, view, camera_x, camera_y);

    fill_rect(renderer, 0, 0, WINDOW_WIDTH, 62, (SDL_Color){0, 0, 0, 140});
    draw_task_bar(renderer, view);
    draw_text(renderer, assets->small_font, "TOTAL TASKS COMPLETED", 30, 42, (SDL_Color){235, 240, 245, 255});
    const ViewPlayer *me_for_ui = local_player(view);
    int is_impostor = me_for_ui && strcmp(me_for_ui->role, "impostor") == 0;
    int can_report = body_near_local_player(view);
    draw_text(renderer, assets->small_font,
              is_impostor ? "Impostor: K kills nearby crewmate | R reports body"
                           : "Crewmate: E task | R report only near body",
              460, 24, (SDL_Color){235, 240, 245, 255});

    draw_button(renderer, assets->font, WINDOW_WIDTH - 162, WINDOW_HEIGHT - 188, 130, 54, "REPORT R",
                can_report ? (SDL_Color){96, 128, 170, 255} : (SDL_Color){70, 74, 82, 255});
    draw_button(renderer, assets->font, WINDOW_WIDTH - 162, WINDOW_HEIGHT - 122, 130, 54, "USE E",
                is_impostor ? (SDL_Color){70, 74, 82, 255} : (SDL_Color){96, 104, 112, 255});
    if (is_impostor) {
        draw_button(renderer, assets->font, WINDOW_WIDTH - 162, WINDOW_HEIGHT - 56, 130, 44, "KILL K",
                    (SDL_Color){172, 42, 50, 255});
    }

    if (strcmp(view->phase, "MEETING") == 0) {
        fill_rect(renderer, 130, 96, 700, 430, (SDL_Color){36, 40, 48, 245});
        draw_text(renderer, assets->large_font, "EMERGENCY MEETING", 230, 120, (SDL_Color){255, 230, 120, 255});
        const ViewPlayer *meeting_me = local_player(view);
        int can_vote = meeting_me && meeting_me->alive && !vote_ui->vote_sent;
        for (int i = 0; i < view->player_count; i++) {
            char row[96];
            snprintf(row, sizeof(row), "%d  %s  %s", view->players[i].id, view->players[i].name,
                     view->players[i].alive ? "alive" : "dead");
            draw_text(renderer, assets->font, row, 230, 195 + i * 34,
                      view->players[i].alive ? (SDL_Color){235, 240, 245, 255} : (SDL_Color){120, 125, 135, 255});
        }
        if (!meeting_me || !meeting_me->alive) {
            draw_text(renderer, assets->font, "You are dead - you cannot vote", 230, 450, (SDL_Color){240, 80, 90, 255});
        } else if (vote_ui->vote_sent) {
            draw_text(renderer, assets->font, "Vote sent. Waiting for others...", 230, 450, (SDL_Color){61, 213, 74, 255});
        } else if (can_vote) {
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "Type alive player ID and press Enter, or 0 to skip: %s",
                     vote_ui->input[0] ? vote_ui->input : "_");
            draw_text(renderer, assets->font, prompt, 230, 450, (SDL_Color){255, 255, 255, 255});
        }
        if (vote_ui->message[0]) {
            draw_text(renderer, assets->small_font, vote_ui->message, 230, 486, (SDL_Color){255, 220, 120, 255});
        }
    }

    if (strcmp(view->phase, "FINISHED") == 0) {
        char text[128];
        snprintf(text, sizeof(text), "%s WIN", view->winner);
        fill_rect(renderer, 250, 220, 460, 160, (SDL_Color){12, 16, 22, 245});
        draw_text(renderer, assets->large_font, text, 320, 270, (SDL_Color){255, 255, 255, 255});
        draw_text(renderer, assets->font, "Press L to return to lobby", 340, 335, (SDL_Color){255, 230, 120, 255});
    }
}

static void render_killed_overlay(SDL_Renderer *renderer, Assets *assets, const ViewState *view) {
    const ViewPlayer *me = local_player(view);
    if (!me || me->alive || strcmp(view->phase, "LOBBY") == 0 || strcmp(view->phase, "FINISHED") == 0) return;
    if (strcmp(view->phase, "MEETING") == 0) return;
    fill_rect(renderer, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, (SDL_Color){0, 0, 0, 245});
    draw_text(renderer, assets->large_font, "YOU'VE BEEN KILLED", 260, 255, (SDL_Color){235, 235, 235, 255});
    draw_text(renderer, assets->font, "Wait for the meeting or game end", 315, 325, (SDL_Color){150, 155, 165, 255});
}

static void render_task_overlay(SDL_Renderer *renderer, Assets *assets, const TaskUi *task) {
    if (task->mode == TASK_UI_NONE) return;

    fill_rect(renderer, 190, 120, 580, 380, (SDL_Color){18, 22, 30, 245});
    SDL_Rect outline = {190, 120, 580, 380};
    set_color(renderer, (SDL_Color){230, 235, 240, 255});
    SDL_RenderDrawRect(renderer, &outline);
    draw_text(renderer, assets->large_font, "TASK", 420, 145, (SDL_Color){255, 255, 255, 255});
    draw_text(renderer, assets->font, task->status, 285, 210, (SDL_Color){235, 240, 245, 255});

    if (task->mode == TASK_UI_PROGRESS) {
        int w = 410;
        int fill = 0;
        if (task->kind == TASK_KIND_CARD) {
            fill = (w * task->card_steps) / 7;
        } else {
            int elapsed = (int)(SDL_GetTicks() - task->started_at);
            fill = elapsed >= TASK_PROGRESS_MS ? w : (w * elapsed) / TASK_PROGRESS_MS;
        }
        fill_rect(renderer, 275, 285, w, 34, (SDL_Color){45, 52, 60, 255});
        fill_rect(renderer, 275, 285, fill, 34, (SDL_Color){61, 213, 74, 255});
        if (task->kind == TASK_KIND_CARD) {
            int card_x = 285 + task->card_steps * 52;
            fill_rect(renderer, 285, 245, 390, 22, (SDL_Color){70, 78, 86, 255});
            fill_rect(renderer, card_x, 228, 86, 54, (SDL_Color){230, 230, 218, 255});
            fill_rect(renderer, card_x + 12, 244, 62, 8, (SDL_Color){60, 80, 115, 255});
            draw_text(renderer, assets->small_font, "CARD task: naciskaj strzalke w prawo", 335, 340, (SDL_Color){190, 205, 215, 255});
        } else {
            draw_text(renderer, assets->small_font, "Progress task: poczekaj az pasek sie wypelni", 305, 340, (SDL_Color){190, 205, 215, 255});
        }
    } else if (task->mode == TASK_UI_SHOW_CODE) {
        draw_text(renderer, assets->large_font, task->code, 425, 280, (SDL_Color){255, 230, 120, 255});
        char series[64];
        snprintf(series, sizeof(series), "Code task: seria %d/3", task->series_done + 1);
        draw_text(renderer, assets->small_font, series, 445, 350, (SDL_Color){190, 205, 215, 255});
    } else if (task->mode == TASK_UI_INPUT_CODE) {
        draw_text(renderer, assets->large_font, task->input[0] ? task->input : "____", 410, 280,
                  (SDL_Color){255, 255, 255, 255});
        draw_text(renderer, assets->small_font, "Wpisuj cyfry z klawiatury", 370, 350, (SDL_Color){190, 205, 215, 255});
    } else if (task->mode == TASK_UI_DONE) {
        draw_text(renderer, assets->font, "Task complete", 395, 292, (SDL_Color){61, 213, 74, 255});
    }
}

static void render_role_overlay(SDL_Renderer *renderer, Assets *assets, const char *role) {
    if (!role || role[0] == '\0' || strcmp(role, "unknown") == 0) return;
    fill_rect(renderer, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, (SDL_Color){0, 0, 0, 235});
    if (strcmp(role, "impostor") == 0) {
        draw_text(renderer, assets->large_font, "IMPOSTOR", 365, 245, (SDL_Color){230, 55, 70, 255});
        draw_text(renderer, assets->font, "Zabij crewmate'ow i nie daj sie wyrzucic", 265, 315, (SDL_Color){235, 240, 245, 255});
    } else {
        draw_text(renderer, assets->large_font, "CREWMATE", 355, 245, (SDL_Color){80, 170, 255, 255});
        draw_text(renderer, assets->font, "Wykonuj zadania i znajdz impostora", 305, 315, (SDL_Color){235, 240, 245, 255});
    }
}

static void render_vote_result_overlay(SDL_Renderer *renderer, Assets *assets, const char *message) {
    if (!message || message[0] == '\0') return;
    fill_rect(renderer, 185, 230, 590, 150, (SDL_Color){10, 12, 18, 238});
    SDL_Rect outline = {185, 230, 590, 150};
    set_color(renderer, (SDL_Color){235, 240, 245, 255});
    SDL_RenderDrawRect(renderer, &outline);
    draw_text(renderer, assets->large_font, "VOTING RESULT", 305, 252, (SDL_Color){255, 230, 120, 255});
    draw_text(renderer, assets->font, message, 245, 320, (SDL_Color){235, 240, 245, 255});
}

static void render(SDL_Renderer *renderer, Assets *assets, const ViewState *view,
                   const TaskUi *task, const VoteUi *vote_ui, int show_role, const char *role,
                   int show_vote_result, const char *vote_result) {
    if (strcmp(view->phase, "LOBBY") == 0) {
        render_lobby(renderer, assets, view);
    } else {
        render_game(renderer, assets, view, vote_ui);
    }
    render_task_overlay(renderer, assets, task);
    render_killed_overlay(renderer, assets, view);
    if (show_role) {
        render_role_overlay(renderer, assets, role);
    }
    if (show_vote_result) {
        render_vote_result_overlay(renderer, assets, vote_result);
    }
    SDL_RenderPresent(renderer);
}

static void handle_task_key(TaskUi *task, SDL_Event *event) {
    if (task->mode == TASK_UI_PROGRESS && task->kind == TASK_KIND_CARD &&
        event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_RIGHT) {
        if (task->card_steps < 7) task->card_steps++;
        if (task->card_steps >= 7) {
            task->mode = TASK_UI_DONE;
            task->started_at = SDL_GetTicks();
            copy_text(task->status, sizeof(task->status), "Card accepted");
        }
        return;
    }

    if (task->mode != TASK_UI_INPUT_CODE) return;

    if (event->type == SDL_TEXTINPUT) {
        int len = (int)strlen(task->input);
        char ch = event->text.text[0];
        if (ch >= '0' && ch <= '9' && len < 4) {
            task->input[len] = ch;
            task->input[len + 1] = '\0';
        }
        if ((int)strlen(task->input) == 4) {
            if (strcmp(task->input, task->code) == 0) {
                task->series_done++;
                if (task->series_done >= 3) {
                    task->mode = TASK_UI_DONE;
                    task->started_at = SDL_GetTicks();
                    copy_text(task->status, sizeof(task->status), "Wszystkie serie poprawne");
                } else {
                    task->mode = TASK_UI_SHOW_CODE;
                    task->started_at = SDL_GetTicks();
                    generate_task_code(task);
                    copy_text(task->status, sizeof(task->status), "Dobrze. Zapamietaj kolejny kod");
                }
            } else {
                task->mode = TASK_UI_SHOW_CODE;
                task->started_at = SDL_GetTicks();
                generate_task_code(task);
                copy_text(task->status, sizeof(task->status), "Zly kod. Nowa proba");
            }
        }
    } else if (event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_BACKSPACE) {
        int len = (int)strlen(task->input);
        if (len > 0) task->input[len - 1] = '\0';
    }
}

static int handle_vote_key(ClientRuntime *runtime, VoteUi *vote_ui, const ViewState *snapshot, SDL_Event *event) {
    const ViewPlayer *me = local_player(snapshot);
    if (strcmp(snapshot->phase, "MEETING") != 0 || !me || !me->alive || vote_ui->vote_sent) return 0;
    if (event->type != SDL_KEYDOWN || event->key.repeat != 0) return 0;

    SDL_Keycode key = event->key.keysym.sym;
    if (key >= SDLK_0 && key <= SDLK_9) {
        int len = (int)strlen(vote_ui->input);
        if (len < 7) {
            vote_ui->input[len] = (char)('0' + (key - SDLK_0));
            vote_ui->input[len + 1] = '\0';
        }
        vote_ui->message[0] = '\0';
        return 1;
    }
    if (key == SDLK_BACKSPACE) {
        int len = (int)strlen(vote_ui->input);
        if (len > 0) vote_ui->input[len - 1] = '\0';
        return 1;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        int target_id = atoi(vote_ui->input);
        if (vote_ui->input[0] == '\0') {
            copy_text(vote_ui->message, sizeof(vote_ui->message), "Type an ID first");
            return 1;
        }
        if (target_id != 0) {
            const ViewPlayer *target = find_view_player(snapshot, target_id);
            if (!target || !target->alive) {
                copy_text(vote_ui->message, sizeof(vote_ui->message), "You can only vote for alive players");
                vote_ui->input[0] = '\0';
                return 1;
            }
        }
        char command[32];
        snprintf(command, sizeof(command), "VOTE %d", target_id);
        net_send_line(runtime->socket_fd, command);
        vote_ui->vote_sent = 1;
        copy_text(vote_ui->message, sizeof(vote_ui->message), "Vote sent");
        return 1;
    }
    return 0;
}

static void handle_action_key(ClientRuntime *runtime, TaskUi *task, const ViewState *snapshot,
                              unsigned int local_completed_tasks, SDL_Keycode key) {
    if (task->mode != TASK_UI_NONE) {
        if (key == SDLK_ESCAPE) {
            task->mode = TASK_UI_NONE;
            SDL_StopTextInput();
        }
        return;
    }

    switch (key) {
        case SDLK_e: {
            const ViewPlayer *me = local_player(snapshot);
            TaskKind kind = me ? task_kind_at(snapshot, me->x, me->y) : TASK_KIND_NONE;
            int task_index = me ? task_index_at(snapshot, me->x, me->y) : -1;
            if (me && strcmp(snapshot->phase, "RUNNING") == 0 &&
                strcmp(me->role, "crewmate") == 0 && kind != TASK_KIND_NONE &&
                task_index >= 0 && !(local_completed_tasks & (1u << task_index))) {
                start_task_ui(task, kind, task_index);
            } else {
                net_send_line(runtime->socket_fd, "TASK");
            }
            break;
        }
        case SDLK_r: net_send_line(runtime->socket_fd, "REPORT"); break;
        case SDLK_x: net_send_line(runtime->socket_fd, "START"); break;
        case SDLK_l: net_send_line(runtime->socket_fd, "RESET"); break;
        case SDLK_v: break;
        case SDLK_k: {
            const ViewPlayer *me = local_player(snapshot);
            if (me && strcmp(me->role, "impostor") == 0) {
                net_send_line(runtime->socket_fd, "KILL");
            }
            break;
        }
        case SDLK_q:
        case SDLK_ESCAPE:
            net_send_line(runtime->socket_fd, "QUIT");
            InterlockedExchange(&runtime->running, 0);
            break;
        default:
            break;
    }
}

static void send_continuous_movement(ClientRuntime *runtime, Uint32 *last_move_time, const TaskUi *task) {
    if (task->mode != TASK_UI_NONE) return;
    Uint32 now = SDL_GetTicks();
    if (now - *last_move_time < MOVE_INTERVAL_MS) return;

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    int dx = 0;
    int dy = 0;
    if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) dy = -1;
    else if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) dy = 1;
    else if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) dx = -1;
    else if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) dx = 1;

    if (dx != 0 || dy != 0) {
        char command[32];
        snprintf(command, sizeof(command), "MOVE %d %d", dx, dy);
        net_send_line(runtime->socket_fd, command);
        *last_move_time = now;
    }
}

int main(int argc, char **argv) {
    char host_buffer[128] = "127.0.0.1";
    char port_buffer[32] = DEFAULT_PORT;
    char menu_choice[16];
    int created_local_lobby = 0;
    PROCESS_INFORMATION server_process;
    memset(&server_process, 0, sizeof(server_process));
    const char *host = argc > 1 ? argv[1] : host_buffer;
    const char *port = argc > 2 ? argv[2] : port_buffer;
    char name[MAX_NAME_LEN + 1];
    char lobby_name[MAX_NAME_LEN + 1] = "Lobby";
    char command[MAX_LINE_LEN];
    char response[MAX_LINE_LEN * 2];

    srand((unsigned int)time(NULL));
    printf("Podaj nazwe gracza: ");
    fflush(stdout);
    if (!fgets(name, sizeof(name), stdin)) return 1;
    name[strcspn(name, "\r\n")] = '\0';
    if (name[0] == '\0') copy_text(name, sizeof(name), "Player");

    if (argc <= 1) {
        printf("Adres serwera [127.0.0.1]: ");
        fflush(stdout);
        if (fgets(host_buffer, sizeof(host_buffer), stdin)) {
            host_buffer[strcspn(host_buffer, "\r\n")] = '\0';
            if (host_buffer[0] == '\0') copy_text(host_buffer, sizeof(host_buffer), "127.0.0.1");
        }
        printf("Port serwera [5050]: ");
        fflush(stdout);
        if (fgets(port_buffer, sizeof(port_buffer), stdin)) {
            port_buffer[strcspn(port_buffer, "\r\n")] = '\0';
            if (port_buffer[0] == '\0') copy_text(port_buffer, sizeof(port_buffer), DEFAULT_PORT);
        }
        printf("1. Stworz nowe lobby\n");
        printf("2. Dolacz do istniejacego lobby\n");
        printf("Wybor: ");
        fflush(stdout);
        if (!fgets(menu_choice, sizeof(menu_choice), stdin)) return 1;
    } else {
        copy_text(menu_choice, sizeof(menu_choice), "2");
    }

    if (!net_startup()) {
        fprintf(stderr, "Nie mozna uruchomic Winsock\n");
        return 1;
    }
    SOCKET socket_fd = net_connect_to_server(host, port);
    if (socket_fd == INVALID_SOCKET && menu_choice[0] == '1' && strcmp(host, "127.0.0.1") == 0) {
        STARTUPINFOA startup;
        char command_line[128];
        memset(&startup, 0, sizeof(startup));
        startup.cb = sizeof(startup);
        snprintf(command_line, sizeof(command_line), "server.exe %s", port_buffer);
        if (CreateProcessA(NULL, command_line, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                           NULL, NULL, &startup, &server_process)) {
            created_local_lobby = 1;
            Sleep(500);
            socket_fd = net_connect_to_server(host, port);
        }
    }
    if (socket_fd == INVALID_SOCKET) {
        fprintf(stderr, "Nie mozna polaczyc z %s:%s\n", host, port);
        net_cleanup();
        return 1;
    }

    read_until_prefix(socket_fd, "INFO", response, sizeof(response));
    if (menu_choice[0] == '1') {
        printf("Nazwa lobby [Lobby]: ");
        fflush(stdout);
        if (fgets(lobby_name, sizeof(lobby_name), stdin)) {
            lobby_name[strcspn(lobby_name, "\r\n")] = '\0';
            if (lobby_name[0] == '\0') copy_text(lobby_name, sizeof(lobby_name), "Lobby");
        }
        snprintf(command, sizeof(command), "CREATE %s %s", lobby_name, name);
        net_send_line(socket_fd, command);
        if (!read_until_prefix(socket_fd, "WELCOME", response, sizeof(response))) {
            fprintf(stderr, "%s\n", response);
            net_close_socket(socket_fd);
            net_cleanup();
            return 1;
        }
        printf("Utworzono i dolaczono: %s\n", response);
    } else {
        net_send_line(socket_fd, "LIST");
        if (read_until_prefix(socket_fd, "LOBBIES", response, sizeof(response))) {
            printf("Dostepne lobby:\n");
            print_lobby_list(response);
        }
        printf("Podaj ID lobby: ");
        fflush(stdout);
        if (!fgets(menu_choice, sizeof(menu_choice), stdin)) {
            net_close_socket(socket_fd);
            net_cleanup();
            return 1;
        }
        menu_choice[strcspn(menu_choice, "\r\n")] = '\0';
        snprintf(command, sizeof(command), "JOIN %s %s", menu_choice, name);
        net_send_line(socket_fd, command);
        if (!read_until_prefix(socket_fd, "WELCOME", response, sizeof(response))) {
            fprintf(stderr, "%s\n", response);
            net_close_socket(socket_fd);
            net_cleanup();
            return 1;
        }
        printf("Dolaczono: %s\n", response);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0 || TTF_Init() != 0) {
        fprintf(stderr, "SDL/TTF init error: %s\n", SDL_GetError());
        net_close_socket(socket_fd);
        net_cleanup();
        return 1;
    }

    Assets assets;
    memset(&assets, 0, sizeof(assets));
    if (!load_assets(&assets)) {
        fprintf(stderr, "Nie mozna zaladowac fontu SDL_ttf\n");
        TTF_Quit();
        SDL_Quit();
        net_close_socket(socket_fd);
        net_cleanup();
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Among Us mini - lobby",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (window && !renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!window || !renderer) {
        fprintf(stderr, "SDL window/renderer error: %s\n", SDL_GetError());
        destroy_assets(&assets);
        TTF_Quit();
        SDL_Quit();
        net_close_socket(socket_fd);
        net_cleanup();
        return 1;
    }
    SDL_SetWindowTitle(window, "Among Us mini | Lobby: WASD move, X start | Game: E task, R report, K kill");

    ClientRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.socket_fd = socket_fd;
    InterlockedExchange(&runtime.running, 1);
    InitializeCriticalSection(&runtime.view_mutex);
    init_view(&runtime.view);
    parse_welcome(&runtime.view, response);

    net_send_line(socket_fd, "STATE");
    CreateThread(NULL, 0, receive_thread, &runtime, 0, NULL);

    Uint32 last_move_time = 0;
    Uint32 role_reveal_until = 0;
    Uint32 vote_result_until = 0;
    char revealed_role[16] = "";
    char vote_result_message[128] = "";
    char previous_phase[24] = "LOBBY";
    int seen_vote_result_version = 0;
    unsigned int local_completed_tasks = 0;
    TaskUi task;
    VoteUi vote_ui;
    memset(&task, 0, sizeof(task));
    memset(&vote_ui, 0, sizeof(vote_ui));

    while (InterlockedCompareExchange(&runtime.running, 1, 1)) {
        ViewState snapshot;
        EnterCriticalSection(&runtime.view_mutex);
        snapshot = runtime.view;
        LeaveCriticalSection(&runtime.view_mutex);

        if (strcmp(previous_phase, "LOBBY") == 0 && strcmp(snapshot.phase, "RUNNING") == 0) {
            local_completed_tasks = 0;
            const ViewPlayer *me = local_player(&snapshot);
            if (me) {
                copy_text(revealed_role, sizeof(revealed_role), me->role);
                role_reveal_until = SDL_GetTicks() + 3000;
            }
        }
        if (strcmp(snapshot.phase, "LOBBY") == 0) {
            local_completed_tasks = 0;
        }
        if (strcmp(previous_phase, "MEETING") != 0 && strcmp(snapshot.phase, "MEETING") == 0) {
            memset(&vote_ui, 0, sizeof(vote_ui));
        }
        if (strcmp(snapshot.phase, "MEETING") != 0) {
            vote_ui.input[0] = '\0';
            vote_ui.vote_sent = 0;
            vote_ui.message[0] = '\0';
        }
        if (snapshot.vote_result_version != seen_vote_result_version) {
            seen_vote_result_version = snapshot.vote_result_version;
            copy_text(vote_result_message, sizeof(vote_result_message), snapshot.vote_result);
            vote_result_until = SDL_GetTicks() + 3500;
        }
        copy_text(previous_phase, sizeof(previous_phase), snapshot.phase);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                net_send_line(runtime.socket_fd, "QUIT");
                InterlockedExchange(&runtime.running, 0);
            } else if (event.type == SDL_TEXTINPUT || event.type == SDL_KEYDOWN) {
                if (handle_vote_key(&runtime, &vote_ui, &snapshot, &event)) {
                    continue;
                }
                handle_task_key(&task, &event);
                if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                    handle_action_key(&runtime, &task, &snapshot, local_completed_tasks, event.key.keysym.sym);
                }
            } else if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
                handle_action_key(&runtime, &task, &snapshot, local_completed_tasks, event.key.keysym.sym);
            }
        }

        advance_task_ui(&task, &runtime, &local_completed_tasks);
        send_continuous_movement(&runtime, &last_move_time, &task);

        EnterCriticalSection(&runtime.view_mutex);
        snapshot = runtime.view;
        LeaveCriticalSection(&runtime.view_mutex);
        render(renderer, &assets, &snapshot, &task, &vote_ui,
               SDL_GetTicks() < role_reveal_until, revealed_role,
               SDL_GetTicks() < vote_result_until, vote_result_message);
        SDL_Delay(16);
    }

    net_close_socket(socket_fd);
    DeleteCriticalSection(&runtime.view_mutex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    destroy_assets(&assets);
    TTF_Quit();
    SDL_Quit();
    net_cleanup();
    if (created_local_lobby && server_process.hProcess) {
        TerminateProcess(server_process.hProcess, 0);
        CloseHandle(server_process.hProcess);
        CloseHandle(server_process.hThread);
    }
    return 0;
}
