#include "myshell.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 4096

int main(void)
{
    int sockfd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    // socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    // server setup
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // connect
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("connect");
        close(sockfd);
        return 1;
    }

    printf("Connected to server.\n");

    while (1)
    {
        // prompt
        printf("$ ");
        fflush(stdout);

        // read input
        if (fgets(buffer, BUFFER_SIZE, stdin) == NULL)
        {
            printf("\n");
            break;
        }

        // send command
        send(sockfd, buffer, strlen(buffer), 0);

        // receive response (until <<END>>)
        while (1)
        {
            memset(response, 0, BUFFER_SIZE);
            int bytes = recv(sockfd, response, BUFFER_SIZE - 1, 0);

            if (bytes <= 0)
            {
                break;
            }

            response[bytes] = '\0';

            // check for END marker
            if (strstr(response, "<<END>>") != NULL)
            {
                // remove marker
                char *pos = strstr(response, "<<END>>");
                *pos = '\0';

                printf("%s", response);
                break;
            }

            printf("%s", response);
        }
    }

    close(sockfd);
    return 0;
}
