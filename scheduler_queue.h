#ifndef SCHEDULER_QUEUE_H
#define SCHEDULER_QUEUE_H

#include "server_shared.h"

typedef enum
{
    TASK_SHELL,
    TASK_DEMO_PROGRAM,
    TASK_UNKNOWN_PROGRAM
} TaskType;

typedef struct Task
{
    int task_id;

    int client_id;
    int client_fd;
    int client_port;
    char client_ip[INET_ADDRSTRLEN];

    char command[BUFFER_SIZE];

    TaskType type;

    int burst_time;        // predicted burst
    int remaining_time;   // used by scheduler
    int round_count;      // how many rounds executed
    int arrival_order;    // FCFS tie-breaker

    struct Task *next;
} Task;

Task *create_task_from_command(ClientContext *ctx, const char *command);

void enqueue_task(Task *task);
Task *dequeue_task(void);

//removing specific task from queue based on task_id
//returns 1 if task was found and removed, 0 if not found
int dequeue_task_by_id(int task_id);

//returning best task in queue based on SJRF priority (without removing)
//caller should call dequeue_task_by_id to actually remove it
Task *peek_best_task_sjrf(int last_selected_task_id);

void remove_tasks_for_client(int client_id);

int queue_is_empty(void);

void print_queue_snapshot(void);

#endif
