#include "event_queue.h"

void queue_init(EventQueue *queue) {
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->closed = false;
    InitializeCriticalSection(&queue->mutex);
    InitializeConditionVariable(&queue->not_empty);
    InitializeConditionVariable(&queue->not_full);
}

void queue_destroy(EventQueue *queue) {
    DeleteCriticalSection(&queue->mutex);
}

bool queue_push(EventQueue *queue, const GameEvent *event) {
    EnterCriticalSection(&queue->mutex);
    while (queue->count == EVENT_QUEUE_CAPACITY && !queue->closed) {
        SleepConditionVariableCS(&queue->not_full, &queue->mutex, INFINITE);
    }
    if (queue->closed) {
        LeaveCriticalSection(&queue->mutex);
        return false;
    }
    queue->events[queue->tail] = *event;
    queue->tail = (queue->tail + 1) % EVENT_QUEUE_CAPACITY;
    queue->count++;
    WakeConditionVariable(&queue->not_empty);
    LeaveCriticalSection(&queue->mutex);
    return true;
}

bool queue_pop(EventQueue *queue, GameEvent *event) {
    EnterCriticalSection(&queue->mutex);
    while (queue->count == 0 && !queue->closed) {
        SleepConditionVariableCS(&queue->not_empty, &queue->mutex, INFINITE);
    }
    if (queue->count == 0 && queue->closed) {
        LeaveCriticalSection(&queue->mutex);
        return false;
    }
    *event = queue->events[queue->head];
    queue->head = (queue->head + 1) % EVENT_QUEUE_CAPACITY;
    queue->count--;
    WakeConditionVariable(&queue->not_full);
    LeaveCriticalSection(&queue->mutex);
    return true;
}

void queue_close(EventQueue *queue) {
    EnterCriticalSection(&queue->mutex);
    queue->closed = true;
    WakeAllConditionVariable(&queue->not_empty);
    WakeAllConditionVariable(&queue->not_full);
    LeaveCriticalSection(&queue->mutex);
}
