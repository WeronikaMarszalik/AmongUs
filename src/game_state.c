#include "game_state.h"
#include <stdio.h>
#include <string.h>

static Player *find_player(GameState *state, int player_id) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected && state->players[i].id == player_id) {
            return &state->players[i];
        }
    }
    return NULL;
}

static const Player *find_player_const(const GameState *state, int player_id) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected && state->players[i].id == player_id) {
            return &state->players[i];
        }
    }
    return NULL;
}

static int active_players(const GameState *state) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected) {
            count++;
        }
    }
    return count;
}

static int alive_crewmates(const GameState *state) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected && state->players[i].alive && state->players[i].role == ROLE_CREWMATE) {
            count++;
        }
    }
    return count;
}

static int alive_impostors(const GameState *state) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected && state->players[i].alive && state->players[i].role == ROLE_IMPOSTOR) {
            count++;
        }
    }
    return count;
}

static int alive_players(const GameState *state) {
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected && state->players[i].alive) {
            count++;
        }
    }
    return count;
}

static bool is_wall_at(int x, int y) {
    for (int i = 0; i < WALL_COUNT; i++) {
        int wx = WALLS[i][0];
        int wy = WALLS[i][1];
        int ww = WALLS[i][2];
        int wh = WALLS[i][3];
        if (x >= wx && x < wx + ww && y >= wy && y < wy + wh) {
            return true;
        }
    }
    return false;
}

static bool is_adjacent_or_same(const Player *a, const Player *b) {
    int dx = a->x - b->x;
    int dy = a->y - b->y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx + dy <= 1;
}

static void clear_bodies(GameState *state) {
    memset(state->bodies, 0, sizeof(state->bodies));
}

static void add_body(GameState *state, const Player *victim) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!state->bodies[i].active) {
            state->bodies[i].active = true;
            state->bodies[i].x = victim->x;
            state->bodies[i].y = victim->y;
            state->bodies[i].victim_id = victim->id;
            return;
        }
    }
}

static bool body_near_player(const GameState *state, const Player *player) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!state->bodies[i].active) continue;
        int dx = state->bodies[i].x - player->x;
        int dy = state->bodies[i].y - player->y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy <= 1) return true;
    }
    return false;
}

static void mark_changed(GameState *state) {
    state->state_version++;
}

static void finish_game(GameState *state, const char *winner) {
    state->phase = PHASE_FINISHED;
    snprintf(state->winner, sizeof(state->winner), "%s", winner);
    mark_changed(state);
}

static void check_win_conditions(GameState *state) {
    if (state->phase != PHASE_RUNNING && state->phase != PHASE_MEETING) {
        return;
    }
    if (active_players(state) > 1 && alive_impostors(state) == 0) {
        finish_game(state, "CREWMATES");
    } else if (alive_impostors(state) >= alive_crewmates(state)) {
        finish_game(state, "IMPOSTORS");
    } else if (state->task_count >= TASKS_TO_WIN) {
        finish_game(state, "CREWMATES_BY_TASKS");
    }
}

static void reset_votes(GameState *state) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        state->players[i].voted = false;
        state->players[i].vote_target = -1;
    }
}

static void reset_players_for_lobby(GameState *state) {
    int slot = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected) {
            state->players[i].alive = true;
            state->players[i].role = ROLE_UNKNOWN;
            state->players[i].voted = false;
            state->players[i].vote_target = -1;
            state->players[i].x = 2 + (slot % 4);
            state->players[i].y = 2 + (slot / 4);
            slot++;
        }
    }
    clear_bodies(state);
}

void game_init(GameState *state) {
    memset(state, 0, sizeof(*state));
    state->phase = PHASE_LOBBY;
    state->next_player_id = 1;
    state->state_version = 1;
    state->winner[0] = '\0';
    for (int i = 0; i < MAX_PLAYERS; i++) {
        state->players[i].id = -1;
        state->players[i].vote_target = -1;
    }
    clear_bodies(state);
}

int game_add_player(GameState *state, int socket_fd, const char *name, char *message, int message_size) {
    if (state->phase != PHASE_LOBBY) {
        snprintf(message, message_size, "ERROR Gra juz trwa albo zostala zakonczona");
        return -1;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!state->players[i].connected) {
            Player *player = &state->players[i];
            player->id = state->next_player_id++;
            player->socket_fd = socket_fd;
            player->connected = true;
            player->alive = true;
            player->voted = false;
            player->vote_target = -1;
            player->x = 2 + (i % 4);
            player->y = 2 + (i / 4);
            player->role = ROLE_UNKNOWN;
            snprintf(player->name, sizeof(player->name), "%s", name[0] ? name : "Player");
            snprintf(message, message_size, "WELCOME %d Witaj %s", player->id, player->name);
            mark_changed(state);
            return player->id;
        }
    }
    snprintf(message, message_size, "ERROR Lobby jest pelne");
    return -1;
}

void game_remove_player(GameState *state, int player_id, char *message, int message_size) {
    Player *player = find_player(state, player_id);
    if (!player) {
        snprintf(message, message_size, "INFO Gracz juz byl odlaczony");
        return;
    }
    snprintf(message, message_size, "INFO %s opuszcza gre", player->name);
    memset(player, 0, sizeof(*player));
    player->id = -1;
    player->vote_target = -1;
    mark_changed(state);
    check_win_conditions(state);
}

void game_start(GameState *state, int player_id, char *message, int message_size) {
    if (!find_player(state, player_id)) {
        snprintf(message, message_size, "ERROR Najpierw dolacz do gry");
        return;
    }
    if (state->phase != PHASE_LOBBY) {
        snprintf(message, message_size, "ERROR Gra nie jest w lobby");
        return;
    }
    int players = active_players(state);
    if (players < MIN_PLAYERS_TO_START) {
        snprintf(message, message_size, "ERROR Potrzeba co najmniej %d graczy", MIN_PLAYERS_TO_START);
        return;
    }
    bool impostor_assigned = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected) {
            state->players[i].alive = true;
            if (players == 1) {
                state->players[i].role = ROLE_CREWMATE;
            } else {
                state->players[i].role = impostor_assigned ? ROLE_CREWMATE : ROLE_IMPOSTOR;
                impostor_assigned = true;
            }
        }
    }
    state->task_count = 0;
    clear_bodies(state);
    state->phase = PHASE_RUNNING;
    state->winner[0] = '\0';
    reset_votes(state);
    snprintf(message, message_size, "INFO Gra wystartowala");
    mark_changed(state);
}

void game_reset_to_lobby(GameState *state, int player_id, char *message, int message_size) {
    if (!find_player(state, player_id)) {
        snprintf(message, message_size, "ERROR Najpierw dolacz do gry");
        return;
    }
    if (state->phase != PHASE_FINISHED) {
        snprintf(message, message_size, "ERROR Do lobby mozna wrocic po zakonczeniu gry");
        return;
    }
    state->phase = PHASE_LOBBY;
    state->task_count = 0;
    state->winner[0] = '\0';
    reset_players_for_lobby(state);
    snprintf(message, message_size, "INFO Powrot do lobby");
    mark_changed(state);
}

void game_move_player(GameState *state, int player_id, int dx, int dy, char *message, int message_size) {
    Player *player = find_player(state, player_id);
    if (!player) {
        snprintf(message, message_size, "ERROR Nieznany gracz");
        return;
    }
    if ((state->phase != PHASE_RUNNING && state->phase != PHASE_LOBBY) ||
        (state->phase == PHASE_RUNNING && !player->alive)) {
        snprintf(message, message_size, "ERROR Ruch niedozwolony w tym stanie");
        return;
    }
    int next_x = player->x + dx;
    int next_y = player->y + dy;
    if (next_x < 0 || next_x >= MAP_WIDTH || next_y < 0 || next_y >= MAP_HEIGHT) {
        snprintf(message, message_size, "ERROR Poza mapa");
        return;
    }
    if (is_wall_at(next_x, next_y)) {
        snprintf(message, message_size, "ERROR Sciana blokuje przejscie");
        return;
    }
    player->x = next_x;
    player->y = next_y;
    snprintf(message, message_size, "INFO %s przesuwa sie na (%d,%d)", player->name, player->x, player->y);
    mark_changed(state);
}

void game_complete_task(GameState *state, int player_id, char *message, int message_size) {
    Player *player = find_player(state, player_id);
    if (!player || state->phase != PHASE_RUNNING || !player->alive || player->role != ROLE_CREWMATE) {
        snprintf(message, message_size, "ERROR Zadanie moze wykonac tylko zywy crewmate podczas gry");
        return;
    }
    bool on_task_spot = false;
    for (int i = 0; i < TASK_SPOT_COUNT; i++) {
        if (TASK_SPOTS[i][0] == player->x && TASK_SPOTS[i][1] == player->y) {
            on_task_spot = true;
            break;
        }
    }
    if (!on_task_spot) {
        snprintf(message, message_size, "ERROR Musisz stac na polu zadania T");
        return;
    }
    state->task_count++;
    snprintf(message, message_size, "INFO %s wykonuje zadanie (%d/%d)", player->name, state->task_count, TASKS_TO_WIN);
    mark_changed(state);
    check_win_conditions(state);
}

void game_report_body(GameState *state, int player_id, char *message, int message_size) {
    Player *player = find_player(state, player_id);
    if (!player || state->phase != PHASE_RUNNING || !player->alive) {
        snprintf(message, message_size, "ERROR Nie mozna zglosic spotkania");
        return;
    }
    if (!body_near_player(state, player)) {
        snprintf(message, message_size, "ERROR Musisz podejsc do ciala");
        return;
    }
    state->phase = PHASE_MEETING;
    clear_bodies(state);
    reset_votes(state);
    snprintf(message, message_size, "INFO %s zglasza spotkanie", player->name);
    mark_changed(state);
}

void game_vote(GameState *state, int player_id, int target_id, char *message, int message_size) {
    Player *player = find_player(state, player_id);
    if (!player || state->phase != PHASE_MEETING || !player->alive) {
        snprintf(message, message_size, "ERROR Glosowanie jest teraz niedozwolone");
        return;
    }
    if (target_id != 0) {
        const Player *target = find_player_const(state, target_id);
        if (!target || !target->alive) {
            snprintf(message, message_size, "ERROR Cel glosu nie istnieje albo nie zyje");
            return;
        }
    }
    player->voted = true;
    player->vote_target = target_id;
    snprintf(message, message_size, "INFO %s oddaje glos", player->name);
    mark_changed(state);

    int votes_ready = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (state->players[i].connected && state->players[i].alive && state->players[i].voted) {
            votes_ready++;
        }
    }
    if (votes_ready != alive_players(state)) {
        return;
    }

    int best_target = 0;
    int best_votes = 0;
    bool tie = false;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        int target = state->players[i].id;
        int count = 0;
        if (!state->players[i].connected || !state->players[i].alive) {
            continue;
        }
        for (int j = 0; j < MAX_PLAYERS; j++) {
            if (state->players[j].connected && state->players[j].alive && state->players[j].vote_target == target) {
                count++;
            }
        }
        if (count > best_votes) {
            best_votes = count;
            best_target = target;
            tie = false;
        } else if (count == best_votes && count > 0) {
            tie = true;
        }
    }

    if (!tie && best_target != 0) {
        Player *ejected = find_player(state, best_target);
        if (ejected) {
            ejected->alive = false;
            snprintf(message, message_size, "INFO %s zostaje wyrzucony", ejected->name);
        }
    } else {
        snprintf(message, message_size, "INFO Glosowanie bez wyrzucenia");
    }
    state->phase = PHASE_RUNNING;
    reset_votes(state);
    mark_changed(state);
    check_win_conditions(state);
}

void game_kill(GameState *state, int player_id, int target_id, char *message, int message_size) {
    Player *killer = find_player(state, player_id);
    Player *target = find_player(state, target_id);
    if (!killer || !target || state->phase != PHASE_RUNNING || !killer->alive || !target->alive) {
        snprintf(message, message_size, "ERROR Zabojstwo niedozwolone");
        return;
    }
    if (killer->role != ROLE_IMPOSTOR || target->role != ROLE_CREWMATE) {
        snprintf(message, message_size, "ERROR Tylko impostor moze zabic crewmate");
        return;
    }
    if (!is_adjacent_or_same(killer, target)) {
        snprintf(message, message_size, "ERROR Cel musi byc obok impostora");
        return;
    }
    target->alive = false;
    add_body(state, target);
    snprintf(message, message_size, "INFO %s zostaje zabity", target->name);
    mark_changed(state);
    check_win_conditions(state);
}

void game_kill_nearby(GameState *state, int player_id, char *message, int message_size) {
    Player *killer = find_player(state, player_id);
    if (!killer || state->phase != PHASE_RUNNING || !killer->alive || killer->role != ROLE_IMPOSTOR) {
        snprintf(message, message_size, "ERROR Zabojstwo niedozwolone");
        return;
    }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        Player *target = &state->players[i];
        if (target->connected && target->alive && target->role == ROLE_CREWMATE &&
            is_adjacent_or_same(killer, target)) {
            game_kill(state, player_id, target->id, message, message_size);
            return;
        }
    }
    snprintf(message, message_size, "ERROR Brak crewmate'a w zasiegu");
}

void game_build_state_message(const GameState *state, char *buffer, int buffer_size) {
    const char *phase = "LOBBY";
    if (state->phase == PHASE_RUNNING) phase = "RUNNING";
    if (state->phase == PHASE_MEETING) phase = "MEETING";
    if (state->phase == PHASE_FINISHED) phase = "FINISHED";

    int written = snprintf(buffer, buffer_size, "STATE version=%d phase=%s map=%dx%d tasks=%d/%d winner=%s taskspots=",
                           state->state_version, phase, MAP_WIDTH, MAP_HEIGHT, state->task_count, TASKS_TO_WIN,
                           state->winner[0] ? state->winner : "-");
    for (int i = 0; i < TASK_SPOT_COUNT && written < buffer_size; i++) {
        int added = snprintf(buffer + written, buffer_size - written,
                             "%d,%d,%c%s", TASK_SPOTS[i][0], TASK_SPOTS[i][1], TASK_TYPES[i],
                             i == TASK_SPOT_COUNT - 1 ? "" : "|");
        if (added < 0) {
            return;
        }
        written += added;
    }
    if (written < buffer_size) {
        int added = snprintf(buffer + written, buffer_size - written, " players=");
        if (added < 0) {
            return;
        }
        written += added;
    }
    for (int i = 0; i < MAX_PLAYERS && written < buffer_size; i++) {
        if (!state->players[i].connected) {
            continue;
        }
        const char *role = state->players[i].role == ROLE_IMPOSTOR ? "impostor" :
                           state->players[i].role == ROLE_CREWMATE ? "crewmate" : "unknown";
        int added = snprintf(buffer + written, buffer_size - written,
                             "%d:%s:%s:%s:(%d,%d)%s",
                             state->players[i].id,
                             state->players[i].name,
                             role,
                             state->players[i].alive ? "alive" : "dead",
                             state->players[i].x,
                             state->players[i].y,
                             i == MAX_PLAYERS - 1 ? "" : ";");
        if (added < 0) {
            return;
        }
        written += added;
    }
    if (written < buffer_size) {
        int added = snprintf(buffer + written, buffer_size - written, " bodies=");
        if (added < 0) return;
        written += added;
    }
    bool first = true;
    for (int i = 0; i < MAX_PLAYERS && written < buffer_size; i++) {
        if (!state->bodies[i].active) continue;
        int added = snprintf(buffer + written, buffer_size - written, "%s%d,%d,%d",
                             first ? "" : "|", state->bodies[i].x,
                             state->bodies[i].y, state->bodies[i].victim_id);
        if (added < 0) return;
        written += added;
        first = false;
    }
}

int game_find_socket_by_player(const GameState *state, int player_id) {
    const Player *player = find_player_const(state, player_id);
    return player ? player->socket_fd : -1;
}
