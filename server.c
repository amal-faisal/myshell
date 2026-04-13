#include "myshell.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>

#define PORT 8080
#define BUFFER_SIZE 4096
#define END_MARKER "<<END>>"
#define MAX_PORT_TRIES 20
#define PORT_HINT_FILE ".myshell_port"

//defining global mutex for serializing server log output across threads
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

//defining global mutex for protecting shared client id assignment
static pthread_mutex_t g_client_id_mutex = PTHREAD_MUTEX_INITIALIZER;

//defining global counter for assigning incremental client ids
static int g_next_client_id = 1;

//serializing process-wide stdio redirection used by builtin execution
static pthread_mutex_t g_builtin_stdio_mutex = PTHREAD_MUTEX_INITIALIZER;

//holding dedicated server logging fd that remains stable across dup2 operations
static int g_server_log_fd = -1;

//holding per-client context passed safely to a worker thread
typedef struct
{
    int client_fd;
    int client_id;
    int client_port;
    char client_ip[INET_ADDRSTRLEN];
    int thread_index;
} ClientContext;

//printing server logs while avoiding interleaving between concurrent threads
static void log_printf_locked(const char *fmt, ...)
{
    char log_buffer[8192];
    int formatted_len;
    va_list args;
    int target_fd;

    pthread_mutex_lock(&g_log_mutex);

    va_start(args, fmt);
    formatted_len = vsnprintf(log_buffer, sizeof(log_buffer), fmt, args);
    va_end(args);

    target_fd = (g_server_log_fd >= 0) ? g_server_log_fd : STDERR_FILENO;
    if (formatted_len > 0)
    {
        size_t to_write = (size_t)formatted_len;
        if (to_write > sizeof(log_buffer))
        {
            to_write = sizeof(log_buffer);
        }

        if (write(target_fd, log_buffer, to_write) < 0)
        {
            perror("write");
        }
    }

    pthread_mutex_unlock(&g_log_mutex);
}

//assigning unique client id in a thread-safe way
static int allocate_client_id(void)
{
    int assigned_id;

    pthread_mutex_lock(&g_client_id_mutex);
    assigned_id = g_next_client_id;
    g_next_client_id++;
    pthread_mutex_unlock(&g_client_id_mutex);

    return assigned_id;
}

//parses a numeric port string and validates range [1, 65535]
//returns 0 on success, -1 on invalid input
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

//same as parse_port_str, but exits with a friendly error message on invalid input
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

//tries to bind starting at start_port; if busy, increments port until max_tries
//returns 0 on success and writes chosen port to bound_port
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

//writes the active server port so the client can auto-discover it later
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

//checks if command exists either as a path or in any directory from PATH
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

//sends the full buffer even if send() writes only partial bytes
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

//sends protocol delimiter so client knows command output is complete
static int send_end_marker(int client_fd)
{
    return send_all(client_fd, END_MARKER, strlen(END_MARKER));
}

//reads child/builtin output from pipe, mirrors it on server logs, and forwards to client
static int stream_and_log_output(int client_fd, int read_fd)
{
    char output_buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    log_printf_locked("[OUTPUT] Sending output to client:\n");

    while ((bytes_read = read(read_fd, output_buffer, BUFFER_SIZE - 1)) > 0)
    {
        output_buffer[bytes_read] = '\0';

        //printing the same output on the server for flow demonstration
        log_printf_locked("%s", output_buffer);

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

//checks if a command has any redirections (input, output, or error)
static int command_has_redirections(Command *cmd)
{
    return (cmd->input_file != NULL || cmd->output_file != NULL || cmd->error_file != NULL);
}

//for single commands only: checks if the command is invalid (non-builtin and not executable)
static int is_invalid_single_command(char *input)
{
    char input_copy[BUFFER_SIZE];
    Command cmd;

    strncpy(input_copy, input, sizeof(input_copy) - 1);
    input_copy[sizeof(input_copy) - 1] = '\0';

    parse_command(input_copy, &cmd);

    //if parser already found a syntax/redirection error, let normal validation
    //path report that exact error instead of overriding it as command-not-found
    if (cmd.has_error)
    {
        return 0;
    }

    //for commands with redirection, let normal execution path handle
    //file checks and command errors in the same order as Phase 1 logic
    if (command_has_redirections(&cmd))
    {
        return 0;
    }

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

//routes command execution through Phase 1 parser/executor logic
//supports both pipelines and non-pipeline commands
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

//executes builtin commands while capturing stdout/stderr into a pipe
//this allows server to send builtin output through the same socket path
//note: builtins with redirections are executed without capture to let redirections apply
static int execute_builtin_in_server_impl(char *input, int client_fd)
{
    Command cmd;
    int pipefd[2];
    int saved_stdout = -1;
    int saved_stderr = -1;

    parse_command(input, &cmd);

    //if builtin has redirections, execute it with proper redirection setup (no output capture)
    //so the redirections (>, <, 2>) apply to files, not to the capture pipe
    if (command_has_redirections(&cmd))
    {
        int saved_stdin = -1;
        int redirect_success = 1;

        //flushing stdio buffers before swapping file descriptors
        fflush(stdout);
        fflush(stderr);

        //handling input redirection
        if (cmd.input_file != NULL)
        {
            saved_stdin = dup(STDIN_FILENO);
            int fd_in = open(cmd.input_file, O_RDONLY);
            if (fd_in == -1)
            {
                perror(cmd.input_file);
                redirect_success = 0;
            }
            else
            {
                dup2(fd_in, STDIN_FILENO);
                close(fd_in);
            }
        }

        //handling output redirection
        if (redirect_success && cmd.output_file != NULL)
        {
            saved_stdout = dup(STDOUT_FILENO);
            int fd_out = open(cmd.output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out == -1)
            {
                perror(cmd.output_file);
                redirect_success = 0;
            }
            else
            {
                dup2(fd_out, STDOUT_FILENO);
                close(fd_out);
            }
        }

        //handling error redirection
        if (redirect_success && cmd.error_file != NULL)
        {
            saved_stderr = dup(STDERR_FILENO);
            int fd_err = open(cmd.error_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_err == -1)
            {
                perror(cmd.error_file);
                redirect_success = 0;
            }
            else
            {
                dup2(fd_err, STDERR_FILENO);
                close(fd_err);
            }
        }

        //executing builtin if redirections succeeded
        if (redirect_success && validate_command(&cmd))
        {
            execute_builtin(&cmd);

            //flushing builtin output while redirections are still active
            fflush(stdout);
            fflush(stderr);
        }

        //restoring original file descriptors
        if (saved_stdin != -1)
        {
            dup2(saved_stdin, STDIN_FILENO);
            close(saved_stdin);
        }
        if (saved_stdout != -1)
        {
            dup2(saved_stdout, STDOUT_FILENO);
            close(saved_stdout);
        }
        if (saved_stderr != -1)
        {
            dup2(saved_stderr, STDERR_FILENO);
            close(saved_stderr);
        }

        if (send_end_marker(client_fd) < 0)
        {
            return -1;
        }

        return 0;
    }

    //no redirections: capture stdout/stderr for sending to client
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

//wrapping builtin execution with mutex because dup2 on stdout/stderr is process-wide
static int execute_builtin_in_server(char *input, int client_fd)
{
    int result;

    pthread_mutex_lock(&g_builtin_stdio_mutex);
    result = execute_builtin_in_server_impl(input, client_fd);
    pthread_mutex_unlock(&g_builtin_stdio_mutex);

    return result;
}

//forks a child to execute Phase 1 logic, captures its output, then streams to client
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

//handling one connected client session until disconnect or explicit exit
static void handle_client_session(ClientContext *ctx)
{
    while (1)
    {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_received;

        //receiving one command line from this client socket
        memset(buffer, 0, BUFFER_SIZE);
        bytes_received = recv(ctx->client_fd, buffer, BUFFER_SIZE - 1, 0);

        if (bytes_received < 0)
        {
            perror("recv");
            break;
        }

        if (bytes_received == 0)
        {
            break;
        }

        //normalizing incoming command by stripping trailing newline
        buffer[bytes_received] = '\0';
        buffer[strcspn(buffer, "\n")] = '\0';

        log_printf_locked("[RECEIVED] Received command: \"%s\" from client.\n", buffer);

        //sending end marker for empty commands to keep protocol synchronized
        if (strlen(buffer) == 0)
        {
            if (send_end_marker(ctx->client_fd) < 0)
            {
                break;
            }
            continue;
        }

        if (strcmp(buffer, "exit") == 0)
        {
            log_printf_locked("[INFO] Client requested exit.\n");
            break;
        }

        log_printf_locked("[EXECUTING] Executing command: \"%s\"\n", buffer);

        //handling obvious invalid single command names early
        if (strchr(buffer, '|') == NULL && is_invalid_single_command(buffer))
        {
            char error_message[BUFFER_SIZE + 30];

            snprintf(error_message, sizeof(error_message), "Command not found: %s\n", buffer);
            log_printf_locked("[ERROR] Command not found: \"%s\"\n", buffer);
            log_printf_locked("[OUTPUT] Sending error message to client: \"Command not found: %s\"\n", buffer);

            if (send_all(ctx->client_fd, error_message, strlen(error_message)) < 0)
            {
                break;
            }

            if (send_end_marker(ctx->client_fd) < 0)
            {
                break;
            }

            continue;
        }

        //handling builtins in-process so shell state behavior remains consistent
        if (strchr(buffer, '|') == NULL)
        {
            char parse_copy[BUFFER_SIZE];
            Command cmd;

            //parsing a copy because parse_command modifies input buffers in-place
            strncpy(parse_copy, buffer, sizeof(parse_copy) - 1);
            parse_copy[sizeof(parse_copy) - 1] = '\0';

            parse_command(parse_copy, &cmd);

            if (cmd.command != NULL && is_builtin(cmd.command))
            {
                if (execute_builtin_in_server(buffer, ctx->client_fd) < 0)
                {
                    break;
                }
                continue;
            }
        }

        //running non-builtin commands through child capture path
        if (execute_in_child_and_stream(buffer, ctx->client_fd) < 0)
        {
            break;
        }
    }
}

//running one thread per client and cleaning resources at thread end
static void *client_worker_thread(void *arg)
{
    ClientContext *ctx = (ClientContext *)arg;

    handle_client_session(ctx);

    close(ctx->client_fd);
    log_printf_locked("[INFO] Client #%d disconnected from %s:%d.\n",
                      ctx->client_id,
                      ctx->client_ip,
                      ctx->client_port);

    free(ctx);
    return NULL;
}

int main(int argc, char **argv)
{
    int server_fd;
    int port = PORT;
    int bound_port = PORT;
    int max_tries = MAX_PORT_TRIES;
    int option = 1;
    struct sockaddr_in address;

    if (argc > 2)
    {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        return 1;
    }

    if (argc == 2)
    {
        //explicit port mode: use exactly this port (no fallback scan)
        port = parse_port_or_exit(argv[1]);
        max_tries = 1;
    }

    //create listening TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return 1;
    }

    //duplicating stderr once for server logging that must survive thread dup2 redirections
    g_server_log_fd = dup(STDERR_FILENO);
    if (g_server_log_fd < 0)
    {
        perror("dup");
        close(server_fd);
        return 1;
    }

    //allow quick restarts even if previous socket is in TIME_WAIT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;

    //try desired/default port first, then fallback ports if needed
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
        log_printf_locked("[INFO] Port %d is busy, using %d instead.\n", port, bound_port);
    }

    //share selected port for clients that auto-read PORT_HINT_FILE
    write_port_hint_file(bound_port);

    //start listening for incoming clients
    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    log_printf_locked("[INFO] Server started, waiting for client connections...\n");

    while (1)
    {
        int client_fd;
        int client_id;
        struct sockaddr_in client_address;
        socklen_t client_address_length = sizeof(client_address);
        ClientContext *ctx;
        pthread_t worker_tid;

        //blocking accept: server stays alive and waits for the next client forever
        client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_address_length);
        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

        //allocating thread context so data remains valid after accept loop iteration
        ctx = (ClientContext *)malloc(sizeof(ClientContext));
        if (ctx == NULL)
        {
            perror("malloc");
            close(client_fd);
            continue;
        }

        client_id = allocate_client_id();

        memset(ctx, 0, sizeof(*ctx));
        ctx->client_fd = client_fd;
        ctx->client_id = client_id;
        ctx->thread_index = client_id;
        ctx->client_port = ntohs(client_address.sin_port);

        if (inet_ntop(AF_INET, &client_address.sin_addr, ctx->client_ip, sizeof(ctx->client_ip)) == NULL)
        {
            strncpy(ctx->client_ip, "unknown", sizeof(ctx->client_ip) - 1);
            ctx->client_ip[sizeof(ctx->client_ip) - 1] = '\0';
        }

        log_printf_locked("[INFO] Client #%d connected from %s:%d. Assigned to Thread-%d.\n",
                          ctx->client_id,
                          ctx->client_ip,
                          ctx->client_port,
                          ctx->thread_index);

        if (pthread_create(&worker_tid, NULL, client_worker_thread, ctx) != 0)
        {
            perror("pthread_create");
            close(client_fd);
            free(ctx);
            continue;
        }

        if (pthread_detach(worker_tid) != 0)
        {
            perror("pthread_detach");
        }
    }

    close(server_fd);
    close(g_server_log_fd);
    return 0;
}
