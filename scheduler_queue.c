#include "scheduler_queue.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Task *queue_head = NULL;
static Task *queue_tail = NULL;

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_not_empty = PTHREAD_COND_INITIALIZER;

static int next_task_id = 1;
static int next_arrival_order = 1;

/* detect: demo N */
static int parse_demo_command(const char *command, int *n_out)
{
    int n;
    char extra[64];

    if (sscanf(command, "demo %d %63s", &n, extra) == 1 && n > 0)
    {
        *n_out = n;
        return 1;
    }

    return 0;
}

Task *create_task_from_command(ClientContext *ctx, const char *command)
{
    Task *task;
    int n = 0;

    if (ctx == NULL || command == NULL)
    {
        return NULL;
    }

    task = (Task *)malloc(sizeof(Task));
    if (task == NULL)
    {
        return NULL;
    }

    memset(task, 0, sizeof(Task));

    pthread_mutex_lock(&queue_mutex);
    task->task_id = next_task_id++;
    task->arrival_order = next_arrival_order++;
    pthread_mutex_unlock(&queue_mutex);

    task->client_id = ctx->client_id;
    task->client_fd = ctx->client_fd;
    task->client_port = ctx->client_port;

    strncpy(task->client_ip, ctx->client_ip, sizeof(task->client_ip) - 1);
    task->client_ip[sizeof(task->client_ip) - 1] = '\0';

    strncpy(task->command, command, sizeof(task->command) - 1);
    task->command[sizeof(task->command) - 1] = '\0';

    if (parse_demo_command(command, &n))
    {
        task->type = TASK_DEMO_PROGRAM;
        task->burst_time = n;
        task->remaining_time = n;
    }
    else
    {
        task->type = TASK_SHELL;
        task->burst_time = -1;
        task->remaining_time = -1;
    }

    task->round_count = 0;
    task->next = NULL;

    return task;
}

void enqueue_task(Task *task)
{
    if (task == NULL)
    {
        return;
    }

    pthread_mutex_lock(&queue_mutex);

    task->next = NULL;

    if (queue_tail == NULL)
    {
        queue_head = task;
        queue_tail = task;
    }
    else
    {
        queue_tail->next = task;
        queue_tail = task;
    }

    pthread_cond_signal(&queue_not_empty);
    pthread_mutex_unlock(&queue_mutex);
}

Task *dequeue_task(void)
{
    Task *task;

    pthread_mutex_lock(&queue_mutex);

    while (queue_head == NULL)
    {
        pthread_cond_wait(&queue_not_empty, &queue_mutex);
    }

    task = queue_head;
    queue_head = queue_head->next;

    if (queue_head == NULL)
    {
        queue_tail = NULL;
    }

    task->next = NULL;

    pthread_mutex_unlock(&queue_mutex);

    return task;
}

void remove_tasks_for_client(int client_id)
{
    Task *curr;
    Task *prev;

    pthread_mutex_lock(&queue_mutex);

    curr = queue_head;
    prev = NULL;

    while (curr != NULL)
    {
        if (curr->client_id == client_id)
        {
            Task *to_delete = curr;

            if (prev == NULL)
            {
                queue_head = curr->next;
                curr = queue_head;
            }
            else
            {
                prev->next = curr->next;
                curr = prev->next;
            }

            if (to_delete == queue_tail)
            {
                queue_tail = prev;
            }

            free(to_delete);
        }
        else
        {
            prev = curr;
            curr = curr->next;
        }
    }

    if (queue_head == NULL)
    {
        queue_tail = NULL;
    }

    pthread_mutex_unlock(&queue_mutex);
}

int queue_is_empty(void)
{
    int empty;

    pthread_mutex_lock(&queue_mutex);
    empty = (queue_head == NULL);
    pthread_mutex_unlock(&queue_mutex);

    return empty;
}

void print_queue_snapshot(void)
{
    Task *curr;

    pthread_mutex_lock(&queue_mutex);

    log_printf_locked("[QUEUE] Current waiting queue:\n");

    if (queue_head == NULL)
    {
        log_printf_locked("[QUEUE]   empty\n");
    }

    curr = queue_head;

    while (curr != NULL)
    {
        log_printf_locked(
            "[QUEUE] Task #%d | Client #%d | cmd=\"%s\" | burst=%d | remaining=%d | round=%d\n",
            curr->task_id,
            curr->client_id,
            curr->command,
            curr->burst_time,
            curr->remaining_time,
            curr->round_count);

        curr = curr->next;
    }

    pthread_mutex_unlock(&queue_mutex);
}
