#include "myshell.h"
#include "server_shared.h"
#include "scheduler_queue.h"
#include "scheduler.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PORT 8080
#define MAX_PORT_TRIES 20
#define PORT_HINT_FILE ".myshell_port"

/* global mutex for logs */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* global mutex for client IDs */
static pthread_mutex_t g_client_id_mutex = PTHREAD_MUTEX_INITIALIZER;

/* next client id */
static int g_next_client_id = 1;

/* stable log fd */
static int g_server_log_fd = -1;

/* ---------- logging ---------- */
void log_printf_locked(const char *fmt, ...)
{
    char buffer[8192];
    va_list args;
    int len;
    int fd;

    pthread_mutex_lock(&g_log_mutex);

    va_start(args, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    fd = (g_server_log_fd >= 0) ? g_server_log_fd : STDERR_FILENO;

    if (len > 0)
    {
        write(fd, buffer, (size_t)len);
    }

    pthread_mutex_unlock(&g_log_mutex);
}

static void handle_sigint(int sig)
{
    (void)sig;
    log_printf_locked("\n[INFO] SIGINT received - printing scheduler summary\n");
    scheduler_print_summary();
    const char *trace = scheduler_get_trace();
    if (trace && trace[0] != '\0')
    {
        log_printf_locked("[TRACE] %s\n", trace);
    }
    exit(0);
}

/* ---------- send helpers ---------- */
int send_all(int sockfd, const char *buffer, size_t length)
{
    size_t sent = 0;

    while (sent < length)
    {
        ssize_t n = send(sockfd, buffer + sent, length - sent, 0);

        if (n < 0)
        {
            perror("send");
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

int send_end_marker(int client_fd)
{
    int result = send_all(client_fd, END_MARKER, strlen(END_MARKER));
    if (result < 0)
        return -1;
    return strlen(END_MARKER);
}

/* ---------- client id ---------- */
static int allocate_client_id(void)
{
    int id;

    pthread_mutex_lock(&g_client_id_mutex);
    id = g_next_client_id++;
    pthread_mutex_unlock(&g_client_id_mutex);

    return id;
}

/* ---------- parse port ---------- */
static int parse_port_or_exit(const char *text)
{
    char *endptr = NULL;
    long port = strtol(text, &endptr, 10);

    if (text == NULL || *text == '\0' || *endptr != '\0' ||
        port < 1 || port > 65535)
    {
        fprintf(stderr, "Invalid port\n");
        exit(1);
    }

    return (int)port;
}

/* ---------- fallback bind ---------- */
static int bind_with_fallback(
    int server_fd,
    struct sockaddr_in *address,
    int start_port,
    int max_tries,
    int *bound_port)
{
    int i;

    for (i = 0; i < max_tries; i++)
    {
        int port = start_port + i;

        address->sin_port = htons(port);

        if (bind(server_fd,
                 (struct sockaddr *)address,
                 sizeof(*address)) == 0)
        {
            *bound_port = port;
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

/* ---------- port hint ---------- */
static void write_port_hint_file(int port)
{
    FILE *fp = fopen(PORT_HINT_FILE, "w");

    if (fp == NULL)
        return;

    fprintf(fp, "%d\n", port);
    fclose(fp);
}

/* ---------- scheduler stub ---------- */
static void *scheduler_thread(void *arg)
{
    (void)arg;

    scheduler_init();

    //getting the scheduler state to track last selected task
    SchedulerState *sched = scheduler_get_state();

    while (1)
    {
        //selecting next task based on SJRF priority
        Task *task = peek_best_task_sjrf(sched->last_selected_task_id);

        if (task == NULL)
        {
            usleep(100000);  //sleeping 100ms to avoid busy-waiting
            continue;
        }

        //removing selected task from queue before executing
        if (!dequeue_task_by_id(task->task_id))
        {
            //task was already removed (shouldn't happen, but handle gracefully)
            continue;
        }

        //mark current task in scheduler state
        scheduler_set_current_task(task);
        scheduler_clear_preempt();

        //logging task execution start
        scheduler_log_decision("started", task);

        //if shell, run to completion
        if (task->type == TASK_SHELL)
        {
            int completed = scheduler_execute_task(task);

            if (completed)
            {
                scheduler_log_decision("ended", task);
                int bytes_sent = send_end_marker(task->client_fd);
                log_printf_locked("[%d]<<< %d bytes sent\n", task->client_id, bytes_sent);

                sched->total_completed++;
                sched->last_selected_task_id = -1;
                free(task);
            }
            scheduler_clear_current_task();
            continue;
        }

        //for demo tasks, run up to quantum_size seconds unless preempted or completed
        int quantum = sched->quantum_size;
        int seconds_run = 0;
        int task_completed = 0;

        for (int q = 0; q < quantum; q++)
        {
            //run one slice (1 second)
            int slice_done = scheduler_execute_task(task);

            //record trace and update quantum counters
            scheduler_append_trace(task->task_id, 1);

            scheduler_add_quantum_consumed(1);

            //update task state after 1s
            scheduler_update_task_after_execution(task, 1);

            seconds_run++;

            if (slice_done)
            {
                task_completed = 1;
                break;
            }

            //check preemption
            int preempt = scheduler_check_preempt();
            if (preempt)
            {
                //log preempted and requeue
                scheduler_log_decision("preempted", task);
                break;
            }
        }

        if (task_completed)
        {
            scheduler_log_decision("ended", task);
            int bytes_sent = send_end_marker(task->client_fd);
            log_printf_locked("[%d]<<< %d bytes sent\n", task->client_id, bytes_sent);

            sched->total_completed++;
            sched->last_selected_task_id = -1;
            free(task);
        }
        else
        {
            //not completed (either preempted or quantum expired) -> requeue
            scheduler_log_decision("waiting", task);
            sched->last_selected_task_id = task->task_id;
            enqueue_task(task);
        }

        scheduler_clear_current_task();
        scheduler_clear_preempt();
    }

    return NULL;
}

/* ---------- client session ---------- */
static void handle_client_session(ClientContext *ctx)
{
    while (1)
    {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_received;
        Task *task;

        memset(buffer, 0, sizeof(buffer));

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

        buffer[bytes_received] = '\0';
        buffer[strcspn(buffer, "\n")] = '\0';

        log_printf_locked("[%d]>>> %s\n", ctx->client_id, buffer);

        if (strlen(buffer) == 0)
        {
            send_end_marker(ctx->client_fd);
            continue;
        }

        if (strcmp(buffer, "exit") == 0)
        {
            log_printf_locked(
                "[INFO] [Client #%d - %s:%d] Client requested disconnect.\n",
                ctx->client_id,
                ctx->client_ip,
                ctx->client_port);
            break;
        }

        task = create_task_from_command(ctx, buffer);

        if (task == NULL)
        {
            const char *msg = "Error: could not create task\n";
            send_all(ctx->client_fd, msg, strlen(msg));
            send_end_marker(ctx->client_fd);
            continue;
        }

        enqueue_task(task);

        log_printf_locked(
            "[%d]=== created (%d)\n",
            ctx->client_id,
            task->burst_time);
    }
}

/* ---------- client worker ---------- */
static void *client_worker_thread(void *arg)
{
    ClientContext *ctx = (ClientContext *)arg;

    handle_client_session(ctx);

    remove_tasks_for_client(ctx->client_id);

    close(ctx->client_fd);

    log_printf_locked("[INFO] Client #%d disconnected.\n\n", ctx->client_id);

    free(ctx);

    return NULL;
}

/* ---------- main ---------- */
int main(int argc, char **argv)
{
    signal(SIGINT, handle_sigint);
    int server_fd;
    int port = PORT;
    int bound_port = PORT;
    int max_tries = MAX_PORT_TRIES;
    int option = 1;
    struct sockaddr_in address;
    pthread_t sched_tid;

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

    g_server_log_fd = dup(STDERR_FILENO);

    if (g_server_log_fd < 0)
    {
        perror("dup");
        close(server_fd);
        return 1;
    }

    if (setsockopt(server_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &option,
                   sizeof(option)) < 0)
    {
        perror("setsockopt");
        close(server_fd);
        close(g_server_log_fd);
        return 1;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind_with_fallback(server_fd,
                           &address,
                           port,
                           max_tries,
                           &bound_port) < 0)
    {
        fprintf(stderr, "Could not bind server socket.\n");
        close(server_fd);
        close(g_server_log_fd);
        return 1;
    }

    if (bound_port != port)
    {
        log_printf_locked(
            "[INFO] Port %d busy, using %d instead.\n",
            port,
            bound_port);
    }

    write_port_hint_file(bound_port);

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        close(g_server_log_fd);
        return 1;
    }

    if (pthread_create(&sched_tid, NULL, scheduler_thread, NULL) != 0)
    {
        perror("pthread_create scheduler");
        close(server_fd);
        close(g_server_log_fd);
        return 1;
    }

    pthread_detach(sched_tid);

    log_printf_locked("[INFO] Server started, waiting for client connections...\n");

    while (1)
    {
        int client_fd;
        int client_id;
        struct sockaddr_in client_address;
        socklen_t client_address_length = sizeof(client_address);
        ClientContext *ctx;
        pthread_t worker_tid;

        client_fd = accept(server_fd,
                           (struct sockaddr *)&client_address,
                           &client_address_length);

        if (client_fd < 0)
        {
            perror("accept");
            continue;
        }

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

        if (inet_ntop(AF_INET,
                      &client_address.sin_addr,
                      ctx->client_ip,
                      sizeof(ctx->client_ip)) == NULL)
        {
            strncpy(ctx->client_ip, "unknown", sizeof(ctx->client_ip) - 1);
            ctx->client_ip[sizeof(ctx->client_ip) - 1] = '\0';
        }

        log_printf_locked(
            "[%d]<<< client connected\n",
            ctx->client_id);

        if (pthread_create(&worker_tid,
                           NULL,
                           client_worker_thread,
                           ctx) != 0)
        {
            perror("pthread_create");
            close(client_fd);
            free(ctx);
            continue;
        }

        pthread_detach(worker_tid);
    }

    close(server_fd);
    close(g_server_log_fd);

    return 0;
}
