#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "scheduler_queue.h"

//holding core scheduler state including current running task and quantum tracking
typedef struct
{
    //currently executing task or null if idle
    Task *current_task;

    //consumed quantum time in current round (0 to quantum_size)
    int quantum_consumed;

    //current round number starting from 1
    int round_number;

    //quantum size for current round (3 for round 1, 7 for later rounds)
    int quantum_size;

    //task id of last selected task to prevent consecutive selection
    int last_selected_task_id;

    //total tasks completed
    int total_completed;

    //total time spent (summing all execution slices)
    int total_time_used;

} SchedulerState;

//initializing scheduler state at startup
void scheduler_init(void);

//selecting next task from queue based on SJRF + RR algorithm
//returns pointer to task or null if queue empty
Task *scheduler_select_next_task(void);

//executing given task for up to quantum milliseconds
//returns 1 if task completed, 0 if task should requeue
int scheduler_execute_task(Task *task);

//updating task state after execution (remaining time, round count)
void scheduler_update_task_after_execution(Task *task, int time_used);

//logging scheduler decision (task selection, preemption, etc)
void scheduler_log_decision(const char *event_type, Task *task);

//printing final summary statistics
void scheduler_print_summary(void);

//getting current scheduler state (read-only)
SchedulerState *scheduler_get_state(void);

//checking if current task should be preempted by new incoming task
//returns 1 if preemption should occur, 0 otherwise
int scheduler_should_preempt(Task *new_task, Task *current_task);

//marking task as preempted and saving state for resume
void scheduler_preempt_task(Task *task);

//marking task as resumed after preemption
void scheduler_resume_task(Task *task);

#endif
