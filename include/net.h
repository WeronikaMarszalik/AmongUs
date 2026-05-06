#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <winsock2.h>

bool net_startup(void);
void net_cleanup(void);
SOCKET net_create_server_socket(const char *port);
SOCKET net_connect_to_server(const char *host, const char *port);
int net_send_line(SOCKET socket_fd, const char *line);
int net_recv_line(SOCKET socket_fd, char *buffer, int buffer_size);
void net_close_socket(SOCKET socket_fd);

#endif
