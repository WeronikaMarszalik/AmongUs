#ifndef GAME_STATE_H
#define GAME_STATE_H

#include <stdbool.h>
#include "config.h"

typedef enum GamePhase {
    PHASE_LOBBY,
    PHASE_RUNNING,
    PHASE_MEETING,
    PHASE_FINISHED
} GamePhase;

typedef enum PlayerRole {
    ROLE_UNKNOWN,
    ROLE_CREWMATE,
    ROLE_IMPOSTOR
} PlayerRole;

typedef struct Player {
    int id;
    int socket_fd;
    bool connected;
    bool alive;
    bool voted;
    int vote_target;
    unsigned int completed_tasks;
    int x;
    int y;
    char name[MAX_NAME_LEN + 1];
    PlayerRole role;
} Player;

typedef struct Body {
    bool active;
    int x;
    int y;
    int victim_id;
} Body;

typedef struct GameState {
    Player players[MAX_PLAYERS];
    Body bodies[MAX_PLAYERS];
    GamePhase phase;
    int next_player_id;
    int task_count;
    int task_goal;
    int state_version;
    char winner[64];
} GameState;

void game_init(GameState *state);
int game_add_player(GameState *state, int socket_fd, const char *name, char *message, int message_size);
void game_remove_player(GameState *state, int player_id, char *message, int message_size);
void game_start(GameState *state, int player_id, char *message, int message_size);
void game_move_player(GameState *state, int player_id, int dx, int dy, char *message, int message_size);
void game_complete_task(GameState *state, int player_id, char *message, int message_size);
void game_report_body(GameState *state, int player_id, char *message, int message_size);
void game_vote(GameState *state, int player_id, int target_id, char *message, int message_size);
void game_kill(GameState *state, int player_id, int target_id, char *message, int message_size);
void game_kill_nearby(GameState *state, int player_id, char *message, int message_size);
void game_reset_to_lobby(GameState *state, int player_id, char *message, int message_size);
void game_build_state_message(const GameState *state, char *buffer, int buffer_size);
int game_find_socket_by_player(const GameState *state, int player_id);

#endif
