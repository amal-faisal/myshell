#include "scheduler.h"
#include "server_shared.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

static SchedulerState g_scheduler = {0};

static pthread_mutex_t scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;

static int g_preempt_flag = 0;
static int g_preempting_task_id = -1;
static char g_trace[4096];
static size_t g_trace_len = 0;

void scheduler_init(void)
{
    pthread_mutex_lock(&scheduler_mutex);

    memset(&g_scheduler, 0, sizeof(SchedulerState));

    g_scheduler.round_number = 1;
    g_scheduler.quantum_size = 3;
    g_scheduler.current_task = NULL;
    g_scheduler.quantum_consumed = 0;
    g_scheduler.last_selected_task_id = -1;
    g_scheduler.total_completed = 0;
    g_scheduler.total_time_used = 0;

    g_preempt_flag = 0;
    g_preempting_task_id = -1;
    g_trace[0] = '\0';
    g_trace_len = 0;

    pthread_mutex_unlock(&scheduler_mutex);
}

Task *scheduler_select_next_task(void)
{
    Task *selected_task = NULL;
    Task *candidate = NULL;
    Task *temp_queue = NULL;
    Task *curr = NULL;
    int queue_size = 0;

    pthread_mutex_lock(&scheduler_mutex);

    if (queue_is_empty())
    {
        pthread_mutex_unlock(&scheduler_mutex);
        return NULL;
    }

    while (!queue_is_empty())
    {
        Task *t = dequeue_task();

        if (t == NULL)
        {
            break;
        }

        t->next = temp_queue;
        temp_queue = t;
        queue_size++;
    }

    curr = temp_queue;

    while (curr != NULL)
    {
        Task *next = curr->next;
        curr->next = NULL;
        enqueue_task(curr);
        curr = next;
    }

    curr = temp_queue;

    while (curr != NULL)
    {
        if (curr->type == TASK_SHELL && curr->burst_time == -1)
        {
            selected_task = curr;
            break;
        }

        curr = curr->next;
    }

    if (selected_task == NULL)
    {
        curr = temp_queue;

        while (curr != NULL)
        {
            if (curr->type == TASK_DEMO_PROGRAM || curr->type == TASK_UNKNOWN_PROGRAM)
            {
                if (candidate == NULL)
                {
                    candidate = curr;
                }
                else if (curr->remaining_time < candidate->remaining_time)
                {
                    candidate = curr;
                }
                else if (curr->remaining_time == candidate->remaining_time &&
                         curr->arrival_order < candidate->arrival_order)
                {
                    candidate = curr;
                }
            }

            curr = curr->next;
        }

        selected_task = candidate;
    }

    if (selected_task != NULL && queue_size > 1)
    {
        if (selected_task->task_id == g_scheduler.last_selected_task_id)
        {
            candidate = NULL;
            curr = temp_queue;

            while (curr != NULL)
            {
                if (curr->task_id != selected_task->task_id &&
                    (curr->type == TASK_DEMO_PROGRAM || curr->type == TASK_UNKNOWN_PROGRAM))
                {
                    if (candidate == NULL)
                    {
                        candidate = curr;
                    }
                    else if (curr->remaining_time < candidate->remaining_time)
                    {
                        candidate = curr;
                    }
                    else if (curr->remaining_time == candidate->remaining_time &&
                             curr->arrival_order < candidate->arrival_order)
                    {
                        candidate = curr;
                    }
                }

                curr = curr->next;
            }

            if (candidate != NULL)
            {
                selected_task = candidate;
            }
        }
    }

    if (selected_task != NULL)
    {
        g_scheduler.last_selected_task_id = selected_task->task_id;
        g_scheduler.quantum_consumed = 0;
    }

    pthread_mutex_unlock(&scheduler_mutex);

    return selected_task;
}

int scheduler_execute_task(Task *task)
{
    if (task == NULL)
    {
        return 0;
    }

    if (task->type == TASK_SHELL)
    {
        int pipefd[2];
        pid_t pid;

        if (pipe(pipefd) == -1)
        {
            perror("pipe");
            return 0;
        }

        pid = fork();

        if (pid < 0)
        {
            perror("fork");
            close(pipefd[0]);
            close(pipefd[1]);
            return 0;
        }

        if (pid == 0)
        {
            close(pipefd[0]);

            if (dup2(pipefd[1], STDOUT_FILENO) == -1)
            {
                perror("dup2 stdout");
                _exit(1);
            }

            if (dup2(pipefd[1], STDERR_FILENO) == -1)
            {
                perror("dup2 stderr");
                _exit(1);
            }

            close(pipefd[1]);

            execlp("/bin/sh", "sh", "-c", task->command, NULL);

            perror("execlp");
            _exit(127);
        }
        else
        {
            char buffer[BUFFER_SIZE];
            ssize_t n;

            close(pipefd[1]);

            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0)
            {
                if (send_all(task->client_fd, buffer, (size_t)n) == 0)
                {
                    task->bytes_sent += (int)n;
                }
            }

            close(pipefd[0]);
            waitpid(pid, NULL, 0);

            return 1;
        }
    }

    if (task->type == TASK_DEMO_PROGRAM)
{
    int current_iteration = task->burst_time - task->remaining_time;

    int sent = dprintf(task->client_fd,
                       "Demo %d/%d\n",
                       current_iteration,
                       task->burst_time);

    if (sent > 0)
    {
        task->bytes_sent += sent;
    }

    if (task->remaining_time > 0)
    {
        sleep(1);
    }

    if (task->remaining_time == 0)
    {
        return 1;
    }

    return 0;
}

    return 0;
}

void scheduler_update_task_after_execution(Task *task, int time_used)
{
    if (task == NULL)
    {
        return;
    }

    pthread_mutex_lock(&scheduler_mutex);

    if (task->remaining_time > 0)
    {
        task->remaining_time -= time_used;

        if (task->remaining_time < 0)
        {
            task->remaining_time = 0;
        }

        g_scheduler.total_time_used += time_used;
    }

    pthread_mutex_unlock(&scheduler_mutex);
}

void scheduler_log_decision(const char *event_type, Task *task)
{
    if (task == NULL || event_type == NULL)
    {
        return;
    }

    if (strcmp(event_type, "created") == 0)
    {
        log_printf_locked("(%d)--- created (%d)\n", task->client_id, task->burst_time);
    }
    else if (strcmp(event_type, "started") == 0)
    {
        log_printf_locked("(%d)--- started (%d)\n", task->client_id, task->remaining_time);
    }
    else if (strcmp(event_type, "waiting") == 0)
    {
        log_printf_locked("(%d)--- waiting (%d)\n", task->client_id, task->remaining_time);
    }
    else if (strcmp(event_type, "running") == 0)
    {
        log_printf_locked("(%d)--- running (%d)\n", task->client_id, task->remaining_time);
    }
    else if (strcmp(event_type, "ended") == 0)
    {
        log_printf_locked("(%d)--- ended (%d)\n", task->client_id, task->remaining_time);
    }
    else if (strcmp(event_type, "preempted") == 0)
    {
        log_printf_locked("(%d)--- preempted (%d)\n", task->client_id, task->remaining_time);
    }
}

void scheduler_print_summary(void)
{
    pthread_mutex_lock(&scheduler_mutex);

    log_printf_locked(
        "=== Scheduler Summary ===\n"
        "Total Completed: %d\n"
        "Total Time Used: %d\n"
        "Final Round: %d\n",
        g_scheduler.total_completed,
        g_scheduler.total_time_used,
        g_scheduler.round_number);

    pthread_mutex_unlock(&scheduler_mutex);
}

SchedulerState *scheduler_get_state(void)
{
    return &g_scheduler;
}

int scheduler_should_preempt(Task *new_task, Task *current_task)
{
    if (current_task == NULL)
    {
        return 0;
    }

    if (current_task->type == TASK_SHELL)
    {
        return 0;
    }

    if (new_task != NULL && new_task->type == TASK_SHELL)
    {
        return 1;
    }

    if (new_task != NULL && current_task != NULL)
    {
        if (new_task->remaining_time < current_task->remaining_time)
        {
            return 1;
        }
    }

    return 0;
}

void scheduler_preempt_task(Task *task)
{
    if (task == NULL)
    {
        return;
    }
}

void scheduler_resume_task(Task *task)
{
    if (task == NULL)
    {
        return;
    }
}

void scheduler_notify_new_task(Task *new_task)
{
    pthread_mutex_lock(&scheduler_mutex);

    if (g_scheduler.current_task != NULL && new_task != NULL)
    {
        if (scheduler_should_preempt(new_task, g_scheduler.current_task))
        {
            g_preempt_flag = 1;
            g_preempting_task_id = new_task->task_id;
        }
    }

    pthread_mutex_unlock(&scheduler_mutex);
}

void scheduler_append_trace(int task_id, int seconds_run)
{
    pthread_mutex_lock(&scheduler_mutex);

    if (g_trace_len + 32 < sizeof(g_trace))
    {
        if (g_trace_len != 0)
        {
            g_trace[g_trace_len++] = '-';
        }

        int n = snprintf(g_trace + g_trace_len,
                         sizeof(g_trace) - g_trace_len,
                         "P%d-(%d)",
                         task_id,
                         seconds_run);

        if (n > 0)
        {
            g_trace_len += (size_t)n;
        }

        g_trace[g_trace_len] = '\0';
    }

    pthread_mutex_unlock(&scheduler_mutex);
}

const char *scheduler_get_trace(void)
{
    return g_trace;
}

void scheduler_set_current_task(Task *task)
{
    pthread_mutex_lock(&scheduler_mutex);
    g_scheduler.current_task = task;
    pthread_mutex_unlock(&scheduler_mutex);
}

void scheduler_clear_current_task(void)
{
    pthread_mutex_lock(&scheduler_mutex);
    g_scheduler.current_task = NULL;
    pthread_mutex_unlock(&scheduler_mutex);
}

int scheduler_check_preempt(void)
{
    int value;

    pthread_mutex_lock(&scheduler_mutex);
    value = g_preempt_flag;
    pthread_mutex_unlock(&scheduler_mutex);

    return value;
}

void scheduler_clear_preempt(void)
{
    pthread_mutex_lock(&scheduler_mutex);
    g_preempt_flag = 0;
    g_preempting_task_id = -1;
    pthread_mutex_unlock(&scheduler_mutex);
}

void scheduler_add_quantum_consumed(int inc)
{
    pthread_mutex_lock(&scheduler_mutex);
    g_scheduler.quantum_consumed += inc;
    pthread_mutex_unlock(&scheduler_mutex);
}
