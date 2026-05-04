#include "scheduler.h"
#include "server_shared.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

//holding global scheduler state
static SchedulerState g_scheduler = {0};

//protecting scheduler state during concurrent updates
static pthread_mutex_t scheduler_mutex = PTHREAD_MUTEX_INITIALIZER;

//preemption flag and trace buffer
//protected by scheduler_mutex
static int g_preempt_flag = 0;
static int g_preempting_task_id = -1;
static char g_trace[4096];
static size_t g_trace_len = 0;

//initializing scheduler state at startup
void scheduler_init(void)
{
    pthread_mutex_lock(&scheduler_mutex);

    memset(&g_scheduler, 0, sizeof(SchedulerState));

    //setting initial round number to 1
    g_scheduler.round_number = 1;

    //setting quantum for round 1 to 3 seconds
    g_scheduler.quantum_size = 3;

    //current task starts as null
    g_scheduler.current_task = NULL;

    //quantum consumed starts at 0
    g_scheduler.quantum_consumed = 0;

    //last selected task id starts at -1 (no task selected yet)
    g_scheduler.last_selected_task_id = -1;

    //total completed starts at 0
    g_scheduler.total_completed = 0;

    //total time used starts at 0
    g_scheduler.total_time_used = 0;

    pthread_mutex_unlock(&scheduler_mutex);
}

//selecting next task from queue based on combined SJRF + RR algorithm
//returns pointer to task or null if queue empty
Task *scheduler_select_next_task(void)
{
    Task *selected_task = NULL;
    Task *candidate = NULL;

    pthread_mutex_lock(&scheduler_mutex);

    //checking if queue is empty
    if (queue_is_empty())
    {
        pthread_mutex_unlock(&scheduler_mutex);
        return NULL;
    }

    //decoding all tasks in queue (read-only snapshot)
    //dequeuing and re-enqueuing to peek
    Task *temp_queue = NULL;
    int queue_size = 0;

    //collecting all tasks temporarily
    while (!queue_is_empty())
    {
        Task *t = dequeue_task();

        if (t == NULL)
            break;

        //linking to temp queue
        t->next = temp_queue;
        temp_queue = t;

        queue_size++;
    }

    //re-enqueuing all tasks (in reverse order now)
    Task *curr = temp_queue;

    while (curr != NULL)
    {
        Task *next = curr->next;

        curr->next = NULL;

        enqueue_task(curr);

        curr = next;
    }

    //now selecting best task based on SJRF + RR
    //priority order:
    // 1. shell commands (burst_time == -1) - highest priority
    // 2. demo tasks with shortest remaining time
    // 3. FCFS tie-breaker on arrival_order
    // 4. prevent consecutive selection of same task

    curr = temp_queue;

    //passing 1: finding shell commands
    while (curr != NULL)
    {
        if (curr->type == TASK_SHELL && curr->burst_time == -1)
        {
            //shell command found - select immediately
            selected_task = curr;

            break;
        }

        curr = curr->next;
    }

    //passing 2: if no shell command, find shortest remaining demo task
    if (selected_task == NULL)
    {
        curr = temp_queue;

        while (curr != NULL)
        {
            if (curr->type == TASK_DEMO_PROGRAM || curr->type == TASK_UNKNOWN_PROGRAM)
            {
                //checking if this is a better candidate
                if (candidate == NULL)
                {
                    candidate = curr;
                }
                else
                {
                    //comparing remaining times (SJRF)
                    if (curr->remaining_time < candidate->remaining_time)
                    {
                        candidate = curr;
                    }
                    else if (curr->remaining_time == candidate->remaining_time)
                    {
                        //tie - using FCFS (arrival_order)
                        if (curr->arrival_order < candidate->arrival_order)
                        {
                            candidate = curr;
                        }
                    }
                }
            }

            curr = curr->next;
        }

        selected_task = candidate;
    }

    //preventing consecutive selection of same task (unless only one task in queue)
    if (selected_task != NULL && queue_size > 1)
    {
        if (selected_task->task_id == g_scheduler.last_selected_task_id)
        {
            //this task was just selected - skip to next best option
            //finding second best
            candidate = NULL;

            curr = temp_queue;

            while (curr != NULL)
            {
                if (curr->task_id != selected_task->task_id)
                {
                    if (curr->type == TASK_DEMO_PROGRAM || curr->type == TASK_UNKNOWN_PROGRAM)
                    {
                        if (candidate == NULL)
                        {
                            candidate = curr;
                        }
                        else
                        {
                            if (curr->remaining_time < candidate->remaining_time)
                            {
                                candidate = curr;
                            }
                            else if (curr->remaining_time == candidate->remaining_time)
                            {
                                if (curr->arrival_order < candidate->arrival_order)
                                {
                                    candidate = curr;
                                }
                            }
                        }
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

    //updating last selected task id
    if (selected_task != NULL)
    {
        g_scheduler.last_selected_task_id = selected_task->task_id;

        //resetting quantum at start of execution
        g_scheduler.quantum_consumed = 0;
    }

    pthread_mutex_unlock(&scheduler_mutex);

    return selected_task;
}

//executing given task for up to quantum milliseconds
//returns 1 if task completed, 0 if task should requeue
int scheduler_execute_task(Task *task)
{
    if (task == NULL)
        return 0;

    //shell commands run to completion
    if (task->type == TASK_SHELL)
    {
        //forking child process to execute shell command
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return 0;
        }

        if (pid == 0)
        {
            //child process - redirecting output to client socket
            if (dup2(task->client_fd, STDOUT_FILENO) == -1)
            {
                perror("dup2 stdout");
                _exit(1);
            }

            if (dup2(task->client_fd, STDERR_FILENO) == -1)
            {
                perror("dup2 stderr");
                _exit(1);
            }

            //executing shell command using /bin/sh
            //this allows for pipes and redirections in the command
            execlp("/bin/sh", "sh", "-c", task->command, NULL);

            //if execlp returns, it failed
            perror("execlp");
            _exit(127);
        }
        else
        {
            //parent process - waiting for child to complete
            int status;
            waitpid(pid, &status, 0);

            //shell task always considered completed after execution
            return 1;
        }
    }

    //demo/program tasks run for quantum or until completion
    if (task->type == TASK_DEMO_PROGRAM)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return 0;
    }

    if (pid == 0)
    {
        int current_iteration = task->burst_time - task->remaining_time;

        dprintf(task->client_fd,
                "Demo %d/%d\n",
                current_iteration,
                task->burst_time);

        if (task->remaining_time > 0)
        {
            sleep(1);
        }

        _exit(0);
    }
    else
    {
        int status;
        waitpid(pid, &status, 0);

        if (task->remaining_time == 0)
        {
            return 1;
        }

        return 0;
    }
}
    {
        //forking child process to run demo task's one-second slice
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            return 0;
        }

        if (pid == 0)
        {
            //child process - running one iteration of demo task
            //calculating which iteration this is (0-based: burst_time - remaining_time)
            int current_iteration = task->burst_time - task->remaining_time;

            //outputting demo progress in format: Demo X/Y
            dprintf(task->client_fd, "Demo %d/%d\n",
                    current_iteration, task->burst_time);

            //sleeping 1 second to simulate work
            sleep(1);

            _exit(0);
        }
        else
        {
            //parent process - waiting for child demo slice to complete
            int status;
            waitpid(pid, &status, 0);

            //detect completion before the update decrements remaining_time:
            //remaining_time <= 1 means this slice is the last one
            if (task->remaining_time <= 1)
            {
                return 1;
            }
            else
            {
                return 0;
            }
        }
    }

    return 0;
}

//updating task state after execution (remaining time, round count)
void scheduler_update_task_after_execution(Task *task, int time_used)
{
    if (task == NULL)
        return;

    pthread_mutex_lock(&scheduler_mutex);

    //decrement remaining time and accumulate total CPU time together so that
    //the final "Demo N/N" slice (remaining == 0 on entry) is not counted in
    //total_time_used; this keeps the execution trace numbers consistent with
    //the expected output (e.g. P6-(22) not P6-(23) for a 12-unit demo)
    if (task->remaining_time > 0)
    {
        task->remaining_time -= time_used;

        if (task->remaining_time < 0)
        {
            task->remaining_time = 0;
        }

        g_scheduler.total_time_used += time_used;
    }

    //round_count is incremented once per quantum run in scheduler_thread,
    //not per second here, so that the per-task quantum (3 first, 7 after)
    //is computed correctly

    pthread_mutex_unlock(&scheduler_mutex);
}

//logging scheduler decision (task selection, preemption, etc)
void scheduler_log_decision(const char *event_type, Task *task)
{
    if (task == NULL || event_type == NULL)
        return;

    //formatting log message based on event type with client ID and remaining_time
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

//printing final summary statistics
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

//getting current scheduler state (read-only)
SchedulerState *scheduler_get_state(void)
{
    return &g_scheduler;
}

//checking if current task should be preempted by new incoming task
//returns 1 if preemption should occur, 0 otherwise
int scheduler_should_preempt(Task *new_task, Task *current_task)
{
    //shell commands are never preempted
    if (current_task == NULL)
        return 0;

    if (current_task->type == TASK_SHELL)
    {
        return 0;
    }

    //shell commands always preempt
    if (new_task != NULL && new_task->type == TASK_SHELL)
    {
        return 1;
    }

    //comparing remaining times for demo tasks
    if (new_task != NULL && current_task != NULL)
    {
        if (new_task->remaining_time < current_task->remaining_time)
        {
            //new task is shorter - preempt
            return 1;
        }
    }

    //no preemption needed
    return 0;
}

//marking task as preempted and saving state for resume
void scheduler_preempt_task(Task *task)
{
    if (task == NULL)
        return;

    //preempted task state is saved in task structure
    //remaining_time already contains the remaining work
}

//marking task as resumed after preemption
void scheduler_resume_task(Task *task)
{
    if (task == NULL)
        return;

    //resuming task simply means selecting it again from queue
    //its remaining_time is already accurate from before preemption
}

//notify scheduler that a new task was enqueued (used to trigger preemption)
void scheduler_notify_new_task(Task *new_task)
{
    pthread_mutex_lock(&scheduler_mutex);

    //if there is a current running task, check if should preempt
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

//append execution trace entry (e.g. "P5-(3)")
void scheduler_append_trace(int task_id, int seconds_run)
{
    pthread_mutex_lock(&scheduler_mutex);

    if (g_trace_len + 32 < sizeof(g_trace))
    {
        if (g_trace_len != 0)
        {
            g_trace[g_trace_len++] = '-';
        }
        int n = snprintf(g_trace + g_trace_len, sizeof(g_trace) - g_trace_len, "P%d-(%d)", task_id, seconds_run);
        if (n > 0) g_trace_len += (size_t)n;
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
    int v;
    pthread_mutex_lock(&scheduler_mutex);
    v = g_preempt_flag;
    pthread_mutex_unlock(&scheduler_mutex);
    return v;
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
