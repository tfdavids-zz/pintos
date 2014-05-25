#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/fixed-point.h"
#include "threads/synch.h"
#include "vm/page.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0                       /* Lowest priority. */
#define PRI_DEFAULT 31                  /* Default priority. */
#define PRI_MAX 63                      /* Highest priority. */

struct child_state
  {
    struct list_elem elem;              /* List element. */
    int exit_status;                    /* Exit status (if applicable) */
    tid_t tid;                          /* tid of child. */
    bool load_success;                  /* True if child loaded successfully*/
    struct semaphore sema;              /* For synch between parent/child */
  };

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    struct file *executable;            /* File that spawned this thread. */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    int eff_priority;                   /* Priority (with donation)*/
    struct list_elem allelem;           /* List element for all threads list. */
    struct list lock_list;              /* List of all locks held by thread. */
    struct lock *blocking_lock;          /* A lock the thread is blocked on */
    int64_t wakeup_time;
    struct list_elem sleepelem;
    struct list_elem elem;              /* List element. */
    int nice;                           /* Niceness of thread, for mlfqs */
    fixed_point_t recent_cpu;           /* Recent cpu recieved, for mlfqs */

#ifdef USERPROG
    /* State for managing children. */
    struct list children;               /* Stores state about threads
                                         * spawned by this thread */
    tid_t parent_tid;                   /* Parent process pid of this process */
    int exit_status;                    /* Exit status (if applicable) */
 
    /* State for managing file descriptors. */
    struct file **fd_table;           /* The table of file descriptors. */
    size_t fd_table_size;             /* The size of the table. */
    size_t fd_table_tail_idx;         /* Highest used fd. */

    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
    struct supp_pt supp_pt;                     /* Supplemental page table. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

struct thread *thread_lookup (tid_t tid);
struct child_state *thread_child_lookup (struct thread *t, tid_t child_tid);

void thread_exit (void) NO_RETURN;
void thread_yield (void);
void thread_yield_if_not_highest (void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);
void thread_calculate_priority (void);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */

