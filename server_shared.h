#ifndef SERVER_SHARED_H
#define SERVER_SHARED_H

#include <stddef.h>
#include <netinet/in.h>

#define BUFFER_SIZE 4096
#define END_MARKER "<<END>>"

typedef struct
{
    int client_fd;
    int client_id;
    int client_port;
    char client_ip[INET_ADDRSTRLEN];
    int thread_index;
} ClientContext;

void log_printf_locked(const char *fmt, ...);
int send_all(int sockfd, const char *buffer, size_t length);
int send_end_marker(int client_fd);

#endif
