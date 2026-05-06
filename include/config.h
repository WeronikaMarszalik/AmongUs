#ifndef CONFIG_H
#define CONFIG_H

#define DEFAULT_PORT "5050"
#define SERVER_BACKLOG 16

#define MAX_PLAYERS 10
#define MAX_LOBBIES 8
#define MIN_PLAYERS_TO_START 1
#define MAX_NAME_LEN 24
#define MAX_LINE_LEN 512

#define MAP_WIDTH 20
#define MAP_HEIGHT 14
#define TASKS_TO_WIN 5
#define TASK_SPOT_COUNT 8
#define WALL_COUNT 28

static const int TASK_SPOTS[TASK_SPOT_COUNT][2] = {
    {3, 2},
    {8, 2},
    {15, 2},
    {5, 6},
    {13, 6},
    {2, 11},
    {10, 11},
    {17, 10}
};

static const char TASK_TYPES[TASK_SPOT_COUNT] = {
    'S', 'C', 'S', 'C', 'P', 'S', 'P', 'C'
};

static const int WALLS[WALL_COUNT][4] = {
    {0, 0, 20, 1},
    {0, 13, 20, 1},
    {0, 0, 1, 14},
    {19, 0, 1, 14},
    {4, 1, 1, 4},
    {9, 1, 1, 3},
    {14, 1, 1, 4},
    {4, 6, 1, 3},
    {9, 5, 1, 4},
    {14, 6, 1, 3},
    {2, 4, 3, 1},
    {6, 4, 3, 1},
    {11, 4, 3, 1},
    {15, 4, 3, 1},
    {2, 9, 3, 1},
    {6, 9, 3, 1},
    {11, 9, 3, 1},
    {15, 9, 3, 1},
    {7, 6, 1, 2},
    {12, 6, 1, 2},
    {5, 11, 4, 1},
    {11, 11, 4, 1},
    {16, 12, 2, 1},
    {2, 12, 2, 1},
    {6, 2, 1, 1},
    {12, 2, 1, 1},
    {6, 7, 1, 1},
    {12, 7, 1, 1}
};

#endif
