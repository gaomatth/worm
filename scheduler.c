#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include "scheduler.h"

#include <assert.h>
#include <curses.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "util.h"

// This is an upper limit on the number of tasks we can create.
#define MAX_TASKS 128

// This is the size of each task's stack memory
#define STACK_SIZE 65536

// These are the macros to keep track of the task's state
/**
 * RUNNING: The task is currently running
 * READY: The task is ready to run
 * SLEEPING: It is sleeping for a set number of milliseconds
 * BLOCKED_BY_TASK: Task has been blocked by a handle
 * BLOCKED_BY_INPUT: There is no input from user so it is blocked.
 * EXITED: It has finished and is now exited.
 * */

#define RUNNING 0
#define READY 1
#define SLEEPING 2
#define BLOCKED_BY_TASK 3
#define BLOCKED_BY_INPUT 4
#define EXITED 5

// This struct will hold the all the necessary information for each task
typedef struct task_info {
  // This field stores all the state required to switch back to this task
  ucontext_t context;

  // This field stores another context. This one is only used when the task
  // is exiting.
  ucontext_t exit_context;

  //   a. Keep track of this task's state.
  //   b. If the task is sleeping, when should it wake up?
  //   c. If the task was blocked by another task, what task was it?
  //   d. Was the task blocked waiting for user input? Once you successfully
  //      read input, you will need to save it here so it can be returned.

  // Fields to keep track of what to do with task

  int current_state;
  size_t wake;
  task_t blocked_task;
  int user_input;

} task_info_t;

int current_task = 0;          // The handle of the currently-executing task
int num_tasks = 1;             // The number of tasks created so far
task_info_t tasks[MAX_TASKS];  // Information for every task

/**
 * Initialize the scheduler. Programs should call this before calling any other
 * functions in this file.
 */
void scheduler_init() {
  // Initialize the state of the scheduler by setting the first task to run

  tasks[0].current_state = RUNNING;
}

/**
 * This function will execute when a task's function returns. This allows you
 * to update scheduler states and start another task. This functionWAIT_TASK is run
 * because of how the contexts are set up in the task_create function.
 */
void task_exit() {
  // Make the current task exit immediately.

  tasks[current_task].current_state = EXITED;
  my_scheduler();  // Asking the scheduler to seek what the next running task should be
}

/**
 * Create a new task and add it to the scheduler.
 *
 * \param handle  The handle for this task will be written to this location.
 * \param fn      The new task will run this function.
 */
void task_create(task_t* handle, task_fn_t fn) {
  // Claim an index for the new task
  int index = num_tasks;
  num_tasks++;

  // Set the task handle to this index, since task_t is just an int
  *handle = index;

  // We're going to make two contexts: one to run the task, and one that runs at the end of the task
  // so we can clean up. Start with the second

  // First, duplicate the current context as a starting point
  getcontext(&tasks[index].exit_context);

  // Set up a stack for the exit context
  tasks[index].exit_context.uc_stack.ss_sp = malloc(STACK_SIZE);
  tasks[index].exit_context.uc_stack.ss_size = STACK_SIZE;

  // Set up a context to run when the task function returns. This should call task_exit.
  makecontext(&tasks[index].exit_context, task_exit, 0);

  // Now we start with the task's actual running context
  getcontext(&tasks[index].context);

  // Allocate a stack for the new task and add it to the context
  tasks[index].context.uc_stack.ss_sp = malloc(STACK_SIZE);
  tasks[index].context.uc_stack.ss_size = STACK_SIZE;

  // Now set the uc_link field, which sets things up so our task will go to the exit context when
  // the task function finishes
  tasks[index].context.uc_link = &tasks[index].exit_context;

  // And finally, set up the context to execute the task function
  makecontext(&tasks[index].context, fn, 0);

  // Initializing the new task as READY
  tasks[index].current_state = READY;
}

/**
 * Wait for a task to finish. If the task has not yet finished, the scheduler should
 * suspend this task and wake it up later when the task specified by handle has exited.
 *
 * \param handle  This is the handle produced by task_create
 */
void task_wait(task_t handle) {
  // We are blocking the task here so that another task can run

  tasks[current_task].current_state = BLOCKED_BY_TASK;
  tasks[current_task].blocked_task = handle;
  my_scheduler();
}

// A scheduler function. Finds the next ready task.
void my_scheduler() {
  // Declare an index and a temporary task_t for context swapping purposes.

  int i = current_task;
  task_t temp;
  while (1) {
    i = (i + 1) % num_tasks;

    // If current_state is READY, then make that task run

    if (tasks[i].current_state == READY) {
      tasks[i].current_state = RUNNING;
      temp = current_task;
      current_task = i;
      break;
    }
    // If the current_state is SLEEPING, we check to see if the time has passed the current_task's
    // wake up time

    if (tasks[i].current_state == SLEEPING) {
      if (time_ms() >= tasks[i].wake) {
        tasks[i].current_state = RUNNING;
        temp = current_task;
        current_task = i;
        break;
      }
    }
    // If the task is blokced by another task, then we check to see if the current_task has exited,
    // if so, then we can run the blocked task.

    if (tasks[i].current_state == BLOCKED_BY_TASK) {
      if (tasks[tasks[i].blocked_task].current_state == EXITED) {
        tasks[i].current_state = RUNNING;
        temp = current_task;
        current_task = i;
        break;
      }
    }
    // Checks to see if the task is blocked by having no input by the user, if there is,
    // then we run the taks[i].

    if (tasks[i].current_state == BLOCKED_BY_INPUT) {
      int arrow = getch();
      if (arrow != ERR) {
        tasks[i].current_state = RUNNING;
        tasks[i].user_input = arrow;
        temp = current_task;
        current_task = i;
        break;
      }
    }
  }
  // Finally, swap the context to the appropriate task.

  swapcontext(&tasks[temp].context, &tasks[i].context);
}
/**
 * The currently-executing task should sleep for a specified time. If that time is larger
 * than zero, the scheduler should suspend this task and run a different task until at least
 * ms milliseconds have elapsed.
 *
 * \param ms  The number of milliseconds the task should sleep.
 */
void task_sleep(size_t ms) {
  tasks[current_task].current_state = SLEEPING;  // Setting the task's current_state to SLEEPING
  tasks[current_task].wake =
      time_ms() + ms;  // Setting the time for the current_task to wake up later
  my_scheduler();
}

/**
 * Read a character from user input. If no input is available, the task should
 * block until input becomes available. The scheduler should run a different
 * task while this task is blocked.
 *
 * \returns The read character code
 */
int task_readchar() {
  // To check for input, call getch(). If it returns ERR, no input was available.
  // Otherwise, getch() will returns the character code that was read.

  int arrow = getch();
  if (arrow == ERR) {
    tasks[current_task].current_state = BLOCKED_BY_INPUT;
    my_scheduler();
  }
  arrow = tasks[current_task].user_input;
  return arrow;
}
