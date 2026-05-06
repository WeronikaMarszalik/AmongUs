#ifndef EVENT_QUEUE_H
#define EVENT_QUEUE_H

#include <stdbool.h>
#include <windows.h>
#include "protocol.h"

#define EVENT_QUEUE_CAPACITY 128

typedef struct EventQueue {
    GameEvent events[EVENT_QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
    bool closed;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;
} EventQueue;

void queue_init(EventQueue *queue);
void queue_destroy(EventQueue *queue);
bool queue_push(EventQueue *queue, const GameEvent *event);
bool queue_pop(EventQueue *queue, GameEvent *event);
void queue_close(EventQueue *queue);

#endif
