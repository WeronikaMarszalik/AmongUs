#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "config.h"

typedef enum EventType {
    EVENT_JOIN,
    EVENT_START,
    EVENT_MOVE,
    EVENT_TASK,
    EVENT_REPORT,
    EVENT_VOTE,
    EVENT_KILL,
    EVENT_RESET,
    EVENT_STATE,
    EVENT_QUIT
} EventType;

typedef struct GameEvent {
    EventType type;
    int player_id;
    int socket_fd;
    int lobby_id;
    int arg1;
    int arg2;
    char text[MAX_NAME_LEN + 1];
} GameEvent;

#endif
