#include "myshell.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define END_MARKER "<<END>>"
#define MAX_PORT_TRIES 20
#define PORT_HINT_FILE ".myshell_port"

static int parse_port_str(const char *port_text, int *port_out)
{
    char *endptr = NULL;
    long parsed = strtol(port_text, &endptr, 10);

    if (port_text == NULL || port_text[0] == '\0' || endptr == NULL || *endptr != '\0' || parsed < 1 || parsed > 65535)
    {
        return -1;
    }

    *port_out = (int)parsed;
    return 0;
}

static int parse_port_or_exit(const char *port_text)
{
    int parsed_port;

    if (parse_port_str(port_text, &parsed_port) != 0)
    {
        fprintf(stderr, "Invalid port '%s'. Use a value between 1 and 65535.\n", port_text);
        exit(1);
    }

    return parsed_port;
}

static int bind_with_fallback(int server_fd, struct sockaddr_in *address, int start_port, int max_tries, int *bound_port)
{
    int attempt;

    for (attempt = 0; attempt < max_tries && (start_port + attempt) <= 65535; attempt++)
    {
        int candidate = start_port + attempt;
        address->sin_port = htons(candidate);

        if (bind(server_fd, (struct sockaddr *)address, sizeof(*address)) == 0)
        {
            *bound_port = candidate;
            return 0;
        }

        if (errno != EADDRINUSE)
        {
            perror("bind");
            return -1;
        }
    }

    return -1;
}

static void write_port_hint_file(int port)
{
    FILE *fp = fopen(PORT_HINT_FILE, "w");

    if (fp == NULL)
    {
        return;
    }

    fprintf(fp, "%d\n", port);
    fclose(fp);
}

static int server_command_exists(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0')
    {
        return 0;
    }

    if (strchr(cmd, '/') != NULL)
    {
        return access(cmd, X_OK) == 0;
    }

    char *path_env = getenv("PATH");
    if (path_env == NULL)
    {
        return 0;
    }

    char path_copy[BUFFER_SIZE];
    strncpy(path_copy, path_env, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    char *dir = strtok(path_copy, ":");
    while (dir != NULL)
    {
        char full_path[BUFFER_SIZE];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);

        if (access(full_path, X_OK) == 0)
        {
            return 1;
        }

        dir = strtok(NULL, ":");
    }

    return 0;
}

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
static int send_end_marker(int client_fd)
{
    return send_all(client_fd, END_MARKER, strlen(END_MARKER));
}
static int stream_and_log_output(int client_fd, int read_fd)
{
    char output_buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    printf("[OUTPUT] Sending output to client:\n");

    while ((bytes_read = read(read_fd, output_buffer, BUFFER_SIZE - 1)) > 0)
    {
        output_buffer[bytes_read] = '\0';

        /* printing the same output on the server for flow demonstration */
        printf("%s", output_buffer);

        if (send_all(client_fd, output_buffer, (size_t)bytes_read) < 0)
        {
            return -1;
        }
    }

    if (bytes_read < 0)
    {
        perror("read");
        return -1;
    }

    return 0;
}
static int is_invalid_single_command(char *input)
{
    char input_copy[BUFFER_SIZE];
    Command cmd;

    strncpy(input_copy, input, sizeof(input_copy) - 1);
    input_copy[sizeof(input_copy) - 1] = '\0';

    parse_command(input_copy, &cmd);

    if (cmd.command == NULL)
    {
        return 0;
    }

    if (is_builtin(cmd.command))
    {
        return 0;
    }

    if (!server_command_exists(cmd.command))
    {
        return 1;
    }

    return 0;
}
static void execute_phase1_logic(char *input)
{
    if (strchr(input, '|') != NULL)
    {
        Pipeline p;
        parse_pipeline(input, &p);

        if (validate_pipeline(&p))
        {
            execute_pipeline(&p);
        }
    }
    else
    {
        Command cmd;
        parse_command(input, &cmd);

        if (validate_command(&cmd))
        {
            if (is_builtin(cmd.command))
            {
                execute_builtin(&cmd);
            }
            else
            {
                execute_command(&cmd);
            }
        }
    }
}
static int execute_builtin_in_server(char *input, int client_fd)
{
    Command cmd;
    int pipefd[2];
    int saved_stdout;
    int saved_stderr;

    parse_command(input, &cmd);

    if (pipe(pipefd) == -1)
    {
        perror("pipe");
        return -1;
    }

    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout == -1)
    {
        perror("dup stdout");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr == -1)
    {
        perror("dup stderr");
        close(saved_stdout);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    fflush(stdout);
    fflush(stderr);

    if (dup2(pipefd[1], STDOUT_FILENO) == -1)
    {
        perror("dup2 stdout");
        close(saved_stdout);
        close(saved_stderr);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (dup2(pipefd[1], STDERR_FILENO) == -1)
    {
        perror("dup2 stderr");
        close(saved_stdout);
        close(saved_stderr);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);

    if (validate_command(&cmd))
    {
        execute_builtin(&cmd);
    }

    fflush(stdout);
    fflush(stderr);

    if (dup2(saved_stdout, STDOUT_FILENO) == -1)
    {
        perror("restore stdout");
    }

    if (dup2(saved_stderr, STDERR_FILENO) == -1)
    {
        perror("restore stderr");
    }

    close(saved_stdout);
    close(saved_stderr);

    if (stream_and_log_output(client_fd, pipefd[0]) < 0)
    {
        close(pipefd[0]);
        return -1;
    }

    close(pipefd[0]);

    if (send_end_marker(client_fd) < 0)
    {
        return -1;
    }

    return 0;
}
static int execute_in_child_and_stream(char *input, int client_fd)
{
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) == -1)
    {
        perror("pipe");
        return -1;
    }

    pid = fork();

    if (pid < 0)
    {
        perror("fork failed");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0)
    {
        close(pipefd[0]);

        if (dup2(pipefd[1], STDOUT_FILENO) == -1)
        {
            perror("dup2 stdout");
            close(pipefd[1]);
            _exit(1);
        }

        if (dup2(pipefd[1], STDERR_FILENO) == -1)
        {
            perror("dup2 stderr");
            close(pipefd[1]);
            _exit(1);
        }

        close(pipefd[1]);

        execute_phase1_logic(input);
        _exit(0);
    }

    close(pipefd[1]);

    if (stream_and_log_output(client_fd, pipefd[0]) < 0)
    {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return -1;
    }

    close(pipefd[0]);
    waitpid(pid, NULL, 0);

    if (send_end_marker(client_fd) < 0)
    {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int server_fd;
    int client_fd;
    int port = PORT;
    int bound_port = PORT;
    int max_tries = MAX_PORT_TRIES;
    int option = 1;
    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);

    if (argc > 2)
    {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }

    if (argc == 2)
    {
        port = parse_port_or_exit(argv[1]);
        max_tries = 1;
    }

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind_with_fallback(server_fd, &address, port, max_tries, &bound_port) < 0)
    {
        if (max_tries == 1 && errno == EADDRINUSE)
        {
            fprintf(stderr, "bind: port %d is already in use.\n", port);
            fprintf(stderr, "Try another port: %s %d\n", argv[0], port + 1);
        }
        else
        {
            fprintf(stderr, "bind: could not find a free port in range %d-%d.\n", port, port + max_tries - 1);
            fprintf(stderr, "Run '%s <port>' to force a specific port.\n", argv[0]);
        }
        close(server_fd);
        return 1;
    }

    if (bound_port != port)
    {
        printf("[INFO] Port %d is busy, using %d instead.\n", port, bound_port);
    }

    write_port_hint_file(bound_port);

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("[INFO] Server started on port %d, waiting for client connections...\n", bound_port);
    while (1)
    {
        client_fd = accept(server_fd, (struct sockaddr *)&address, &address_length);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        printf("[INFO] Client connected.\n");

        while (1)
        {
            char buffer[BUFFER_SIZE];
            ssize_t bytes_received;

            memset(buffer, 0, BUFFER_SIZE);
            bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

            if (bytes_received < 0)
            {
                perror("recv");
                break;
            }

            if (bytes_received == 0)
            {
                printf("[INFO] Client disconnected.\n");
                break;
            }

            buffer[bytes_received] = '\0';
            buffer[strcspn(buffer, "\n")] = '\0';

            printf("[RECEIVED] Received command: \"%s\" from client.\n", buffer);

            if (strlen(buffer) == 0)
            {
                if (send_end_marker(client_fd) < 0)
                {
                    break;
                }
                continue;
            }

            if (strcmp(buffer, "exit") == 0)
            {
                printf("[INFO] Client requested exit.\n");
                break;
            }

            printf("[EXECUTING] Executing command: \"%s\"\n", buffer);
            if (strchr(buffer, '|') == NULL && is_invalid_single_command(buffer))
            {
                char error_message[BUFFER_SIZE + 30];

                snprintf(error_message, sizeof(error_message), "Command not found: %s\n", buffer);
                printf("[ERROR] Command not found: \"%s\"\n", buffer);
                printf("[OUTPUT] Sending error message to client: \"Command not found: %s\"\n", buffer);

                if (send_all(client_fd, error_message, strlen(error_message)) < 0)
                {
                    break;
                }

                if (send_end_marker(client_fd) < 0)
                {
                    break;
                }

                continue;
            }
            if (strchr(buffer, '|') == NULL)
            {
                char parse_copy[BUFFER_SIZE];
                Command cmd;

                strncpy(parse_copy, buffer, sizeof(parse_copy) - 1);
                parse_copy[sizeof(parse_copy) - 1] = '\0';

                parse_command(parse_copy, &cmd);

                if (cmd.command != NULL && is_builtin(cmd.command))
                {
                    if (execute_builtin_in_server(buffer, client_fd) < 0)
                    {
                        break;
                    }
                    continue;
                }
            }

            if (execute_in_child_and_stream(buffer, client_fd) < 0)
            {
                break;
            }
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}
