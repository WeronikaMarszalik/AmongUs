CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -Iinclude
LDFLAGS=-lws2_32
SDL_CFLAGS=-IC:/msys64/ucrt64/include/SDL2
SDL_LDFLAGS=-LC:/msys64/ucrt64/lib -lmingw32 -lSDL2main -lSDL2_ttf -lSDL2 -lws2_32

SERVER_OBJS=src/server.o src/game_state.o src/event_queue.o src/net.o
CLIENT_OBJS=src/client.o src/net.o
SDL_CLIENT_OBJS=src/sdl_client.o src/net.o

.PHONY: all clean run-server run-client

all: server.exe client.exe sdl_client.exe

server.exe: $(SERVER_OBJS)
	$(CC) $(SERVER_OBJS) -o $@ $(LDFLAGS)

client.exe: $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o $@ $(LDFLAGS)

sdl_client.exe: $(SDL_CLIENT_OBJS)
	$(CC) $(SDL_CLIENT_OBJS) -o $@ $(SDL_LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/sdl_client.o: src/sdl_client.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

run-server: server.exe
	./server.exe

run-client: client.exe
	./client.exe 127.0.0.1 5050

run-sdl-client: sdl_client.exe
	./sdl_client.exe 127.0.0.1 5050

clean:
	del /Q src\*.o server.exe client.exe sdl_client.exe 2>NUL || exit 0
