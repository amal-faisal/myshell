#include "myshell.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define END_MARKER "<<END>>"

static int parse_port_or_exit(const char *port_text)
{
    char *endptr = NULL;
    long parsed = strtol(port_text, &endptr, 10);

    if (port_text[0] == '\0' || endptr == NULL || *endptr != '\0' || parsed < 1 || parsed > 65535)
    {
        fprintf(stderr, "Invalid port '%s'. Use a value between 1 and 65535.\n", port_text);
        exit(1);
    }

    return (int)parsed;
}

//sending all bytes in a buffer over the socket
//returns 0 on success, -1 on failure
static int send_all(int sockfd, const char *buffer, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length)
    {
        ssize_t bytes_sent = send(sockfd, buffer + total_sent, length - total_sent, 0);
        if (bytes_sent < 0)
        {
            perror("send");
            return -1;
        }

        total_sent += (size_t)bytes_sent;
    }

    return 0;
}

//receiving full server response until END_MARKER appears
//this function handles partial recv() calls and marker splits across packets
//returns 0 on success, -1 on socket/error conditions
static int receive_response_until_end(int sockfd)
{
    char chunk[BUFFER_SIZE];
    size_t used = 0;
    size_t capacity = BUFFER_SIZE;
    size_t marker_len = strlen(END_MARKER);
    char *response = malloc(capacity);

    if (response == NULL)
    {
        perror("malloc");
        return -1;
    }

    response[0] = '\0';

    while (1)
    {
        ssize_t bytes_received = recv(sockfd, chunk, sizeof(chunk) - 1, 0);

        if (bytes_received < 0)
        {
            perror("recv");
            free(response);
            return -1;
        }

        if (bytes_received == 0)
        {
            //server closed connection unexpectedly while waiting for response
            fprintf(stderr, "Server disconnected.\n");
            free(response);
            return -1;
        }

        //ensuring enough storage for newly received bytes plus null terminator
        if (used + (size_t)bytes_received + 1 > capacity)
        {
            while (used + (size_t)bytes_received + 1 > capacity)
            {
                capacity *= 2;
            }

            char *grown = realloc(response, capacity);
            if (grown == NULL)
            {
                perror("realloc");
                free(response);
                return -1;
            }

            response = grown;
        }

        //appending newly received bytes to accumulated response
        memcpy(response + used, chunk, (size_t)bytes_received);
        used += (size_t)bytes_received;
        response[used] = '\0';

        //checking if END_MARKER already arrived in the accumulated buffer
        char *marker_pos = strstr(response, END_MARKER);
        if (marker_pos != NULL)
        {
            //truncating marker so client prints only command output
            *marker_pos = '\0';
            printf("%s", response);
            fflush(stdout);
            free(response);
            return 0;
        }

        //minor optimization: if very small response and no marker yet,
        //continue reading additional chunks
        if (used < marker_len)
        {
            continue;
        }
    }
}

int main(int argc, char **argv)
{
    int sockfd;
    int port = PORT;
    struct sockaddr_in server_addr;
    char input_buffer[BUFFER_SIZE];

    if (argc > 2)
    {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }

    if (argc == 2)
    {
        port = parse_port_or_exit(argv[1]);
    }

    //creating TCP socket for client-server communication
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    //preparing server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    //connecting to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        if (errno == ECONNREFUSED)
        {
            fprintf(stderr, "connect: no server is listening on 127.0.0.1:%d\n", port);
            fprintf(stderr, "Start server first, or run both with the same custom port.\n");
        }
        else
        {
            perror("connect");
        }
        close(sockfd);
        return 1;
    }

    //main client loop: prompt -> read input -> send -> receive -> display
    while (1)
    {
        //displaying prompt similar to regular shell
        printf("$ ");
        fflush(stdout);

        //reading user input from stdin
        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL)
        {
            //EOF (Ctrl+D) or input stream closed
            printf("\n");
            break;
        }

        //sending full command (including newline) to server
        if (send_all(sockfd, input_buffer, strlen(input_buffer)) < 0)
        {
            break;
        }

        //if user asked to exit, server will close this client session
        //so client also exits after sending exit command
        if (strcmp(input_buffer, "exit\n") == 0 || strcmp(input_buffer, "exit") == 0)
        {
            break;
        }

        //receiving full command output until protocol END_MARKER
        if (receive_response_until_end(sockfd) < 0)
        {
            break;
        }
    }

    //closing socket before termination
    close(sockfd);
    return 0;
}
