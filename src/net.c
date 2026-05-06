#include "net.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <ws2tcpip.h>

bool net_startup(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void net_cleanup(void) {
    WSACleanup();
}

SOCKET net_create_server_socket(const char *port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    SOCKET server_socket = INVALID_SOCKET;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, port, &hints, &result) != 0) {
        return INVALID_SOCKET;
    }

    server_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (server_socket == INVALID_SOCKET) {
        freeaddrinfo(result);
        return INVALID_SOCKET;
    }

    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    if (bind(server_socket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        closesocket(server_socket);
        freeaddrinfo(result);
        return INVALID_SOCKET;
    }

    freeaddrinfo(result);
    if (listen(server_socket, SERVER_BACKLOG) == SOCKET_ERROR) {
        closesocket(server_socket);
        return INVALID_SOCKET;
    }
    return server_socket;
}

SOCKET net_connect_to_server(const char *host, const char *port) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ptr = NULL;
    SOCKET client_socket = INVALID_SOCKET;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        return INVALID_SOCKET;
    }

    for (ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        client_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (client_socket == INVALID_SOCKET) {
            continue;
        }
        if (connect(client_socket, ptr->ai_addr, (int)ptr->ai_addrlen) == 0) {
            break;
        }
        closesocket(client_socket);
        client_socket = INVALID_SOCKET;
    }

    freeaddrinfo(result);
    return client_socket;
}

int net_send_line(SOCKET socket_fd, const char *line) {
    int length = (int)strlen(line);
    int sent = send(socket_fd, line, length, 0);
    if (sent == SOCKET_ERROR) {
        return -1;
    }
    if (length == 0 || line[length - 1] != '\n') {
        sent = send(socket_fd, "\n", 1, 0);
        if (sent == SOCKET_ERROR) {
            return -1;
        }
    }
    return 0;
}

int net_recv_line(SOCKET socket_fd, char *buffer, int buffer_size) {
    int index = 0;
    while (index < buffer_size - 1) {
        char ch;
        int received = recv(socket_fd, &ch, 1, 0);
        if (received <= 0) {
            return received;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        buffer[index++] = ch;
    }
    buffer[index] = '\0';
    return index;
}

void net_close_socket(SOCKET socket_fd) {
    if (socket_fd != INVALID_SOCKET) {
        shutdown(socket_fd, SD_BOTH);
        closesocket(socket_fd);
    }
}
