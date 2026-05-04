#include "scheduler_queue.h"
#include "scheduler.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

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
    //notify scheduler that a new task arrived (may cause preemption)
    //scheduler_notify_new_task is defined in scheduler.c
    scheduler_notify_new_task(task);
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

//removing specific task from queue by task_id
//returns 1 if task was removed, 0 if not found  
int dequeue_task_by_id(int task_id)
{
    pthread_mutex_lock(&queue_mutex);

    Task *curr = queue_head;
    Task *prev = NULL;

    //finding the task with matching task_id
    while (curr != NULL)
    {
        if (curr->task_id == task_id)
        {
            //found it - removing from queue
            if (prev == NULL)
            {
                //removing from head
                queue_head = curr->next;
            }
            else
            {
                //removing from middle
                prev->next = curr->next;
            }

            //updating tail if necessary
            if (queue_tail == curr)
            {
                queue_tail = prev;
            }

            curr->next = NULL;

            pthread_mutex_unlock(&queue_mutex);
            return 1;
        }

        prev = curr;
        curr = curr->next;
    }

    //task not found
    pthread_mutex_unlock(&queue_mutex);
    return 0;
}

//returning best task based on SJRF priority (without removing it)
Task *peek_best_task_sjrf(int last_selected_task_id)
{
    pthread_mutex_lock(&queue_mutex);

    Task *selected = NULL;
    Task *curr = queue_head;

    //pass 1: looking for shell commands (highest priority)
    while (curr != NULL)
    {
        if (curr->type == TASK_SHELL && curr->burst_time == -1)
        {
            selected = curr;
            pthread_mutex_unlock(&queue_mutex);
            return selected;
        }
        curr = curr->next;
    }

    //pass 2: looking for demo with shortest remaining time
    //using FCFS (arrival_order) for tie-breaking
    int shortest_time = INT_MAX;
    curr = queue_head;

    while (curr != NULL)
    {
        if (curr->type == TASK_DEMO_PROGRAM)
        {
            //skip if this is the same task as last selection (unless it's the only one)
            if (curr->task_id == last_selected_task_id && queue_head->next != NULL)
            {
                curr = curr->next;
                continue;
            }

            //selecting if shorter, or same time but earlier arrival
            if (selected == NULL || 
                curr->remaining_time < shortest_time ||
                (curr->remaining_time == shortest_time && curr->arrival_order < selected->arrival_order))
            {
                shortest_time = curr->remaining_time;
                selected = curr;
            }
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&queue_mutex);
    return selected;
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
