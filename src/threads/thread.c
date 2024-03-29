#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "devices/timer.h"
#include "threads/fixed-point.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b
#define NUM_PRIO 64
#define INIT_THREAD_NICE 0
#define INIT_RECENT_CPU 0
#define INIT_FDTABE_SIZE 32

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_lists[NUM_PRIO];

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/* Load avg of system, ignored if thread_mlfqs not set. */
static fixed_point_t load_avg;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void ready_lists_insert(struct thread *);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
void thread_calculate_priority (void);
bool thread_has_highest_priority (struct thread *t);

static void recompute_priority_mlfqs (struct thread *t, void *aux);
static void recompute_recent_cpu_mlfqs (struct thread *t, void *aux);
static void recompute_load_avg_mlfqs (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  int i;
  for (i = 0; i < NUM_PRIO; i++) {
    list_init (&ready_lists[i]);
  }
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

  /* Init load_avg for system */
  load_avg = fix_int(0);
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Recompute priority/recent_cpu/load_avg if mlfqs */
  if(thread_mlfqs)
    {
      int curr_timer_ticks = timer_ticks ();

      /* Recompute recent_cpu of all threads and 
       * load_avg for the sys once per sec */
      if (curr_timer_ticks % TIMER_FREQ == 0)
        {
          recompute_load_avg_mlfqs ();
          thread_foreach (recompute_recent_cpu_mlfqs, NULL);
        }

      /* Increment recent_cpu of running thread (unless idle) */
      if (t != idle_thread)
        {
          t->recent_cpu = fix_add (t->recent_cpu, fix_int(1));
        }

      /* Recompute all  priorities once every 4 ticks */
      if (curr_timer_ticks % 4 == 0)
        {
	      if (curr_timer_ticks % TIMER_FREQ == 0)
	        thread_foreach (recompute_priority_mlfqs, NULL);
	      else
	        recompute_priority_mlfqs (t, NULL);
        }

      /* Enforce preemption if thread no longer has highest priority */
      if (!thread_has_highest_priority (t))
	      intr_yield_on_return ();
    }
  
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

/* Only (non-idle) user processes need access to store child state and
   file descriptors. */
#ifdef USERPROG
  struct thread *cur = thread_current ();
  if (function != idle)
    {
      /* Handle child/parent process issues */
      t->parent_tid = cur->tid;

      /* Create struct child_state representing this child thread */
      struct child_state *cs = (struct child_state *)
        malloc (sizeof (struct child_state));
      if (cs == NULL)
        {
          return TID_ERROR;       
        }

      cs->exit_status = -1; /* By default, assume error. */
      cs->tid = tid;
      cs->load_success = false;
      sema_init (&cs->sema, 0);
      list_push_back (&cur->children, &cs->elem);

      /* Set up the file descriptor table. */
      t->fd_table_size = INIT_FDTABE_SIZE;
      t->fd_table = calloc (t->fd_table_size, sizeof (struct file *));
      if (t->fd_table == NULL)
        {
          return TID_ERROR;
        }
      t->fd_table_tail_idx = 1;
    }
#endif

  /* Yield current thread if not highest. */
  enum intr_level old_level = intr_disable ();
  thread_yield_if_not_highest ();
  intr_set_level (old_level);

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);

  ready_lists_insert (t);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    ready_lists_insert (cur);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Yields the CPU if the current thread does not hold the highest
   eff_priority*/
void
thread_yield_if_not_highest (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  if (!thread_has_highest_priority (thread_current ()))
    thread_yield ();
}

/* Returns true if the thread has highest priority among waiting
   threads*/
bool
thread_has_highest_priority (struct thread *t)
{
  ASSERT (intr_get_level () == INTR_OFF);

  int i;
  for (i = t->eff_priority + 1; i < NUM_PRIO; i++)
    {
      if (!list_empty (&ready_lists[i]))
        return false;
    }

  return true;
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs)
    return;

  struct thread *curr = thread_current ();
  curr->priority = new_priority;

  enum intr_level old_level;
  old_level = intr_disable ();

  thread_calculate_priority ();
  thread_yield_if_not_highest ();
  intr_set_level (old_level);
}

void
thread_calculate_priority(void)
{
  ASSERT  (intr_get_level () == INTR_OFF);

  struct thread *curr_thread = thread_current ();
  curr_thread->eff_priority = curr_thread->priority;
  struct list_elem *e;
  for (e = list_begin (&curr_thread->lock_list);
       e != list_end (&curr_thread->lock_list); e = list_next (e))
    {
      struct lock *l = list_entry (e, struct lock, elem);
      if (l->priority > curr_thread->eff_priority)
        {
          curr_thread->eff_priority = l->priority;
        }
    }
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->eff_priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  struct thread *current_thread = thread_current ();
  enum intr_level old_level = intr_disable ();

  current_thread->nice = nice;
  recompute_priority_mlfqs (current_thread, NULL);
  thread_yield_if_not_highest ();

  intr_set_level (old_level);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  struct thread *current_thread = thread_current ();
  return current_thread->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  enum intr_level old_level;
  old_level = intr_disable ();

  fixed_point_t hundred = fix_int (100);
  int curr_load_avg = fix_trunc (fix_mul (hundred, load_avg));
  intr_set_level (old_level);
  return curr_load_avg;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  enum intr_level old_level;
  old_level = intr_disable ();

  fixed_point_t hundred = fix_int (100);
  int curr_recent_cpu = fix_trunc (fix_mul (hundred,
					  thread_current ()->recent_cpu));
  intr_set_level (old_level);
  return curr_recent_cpu;
}

void
recompute_priority_mlfqs (struct thread *t, void *aux UNUSED)
{
  ASSERT (intr_get_level () == INTR_OFF);

  fixed_point_t nfour = fix_int (-4);
  
  t->eff_priority = PRI_MAX + fix_trunc (fix_div (t->recent_cpu, nfour)) -
    t->nice * 2;
  if (t->eff_priority < PRI_MIN)
    {
      t->eff_priority = PRI_MIN;
    }
  else if (t->eff_priority > PRI_MAX)
    {
      t->eff_priority = PRI_MAX;
    }
  /* If in ready_lists, move to the appropriate one */
  if (t->status == THREAD_READY)
    {
      list_remove (&t->elem);
      ready_lists_insert (t);
    }    
}

void
recompute_recent_cpu_mlfqs (struct thread *t, void *aux UNUSED)
{
  ASSERT (intr_get_level () == INTR_OFF);

  fixed_point_t one = fix_int (1);
  fixed_point_t two = fix_int (2);
  t->recent_cpu = fix_add(
                    fix_mul(
                      fix_div(
                        fix_mul (two, load_avg),
                        fix_add (fix_mul (two, load_avg),
                          one)), 
                      t->recent_cpu),
                    fix_int (t->nice));
}

void
recompute_load_avg_mlfqs (void)
{
  ASSERT (intr_get_level () == INTR_OFF);

  fixed_point_t one = fix_int (1);
  fixed_point_t fifty_nine = fix_int (59);
  fixed_point_t sixty = fix_int (60);

  int ready_threads = 0;
  int i;
  for (i = 0; i < NUM_PRIO; i++){
    ready_threads += list_size (&ready_lists[i]);
  }
  /* Do not count the idle thread */
  if (idle_thread->status == THREAD_READY)
    ready_threads--;
  
  if (thread_current () != idle_thread)
    ready_threads++;

  load_avg = fix_add (
         fix_mul (
           fix_div (fifty_nine, sixty),
           load_avg),
         fix_mul (
           fix_div (one, sixty),
           fix_int (ready_threads))
         );
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->eff_priority = priority;
  t->magic = THREAD_MAGIC;
  t->exit_status = -1; /* By default, assume error. */
  if (thread_mlfqs)
    {
      if (t == initial_thread)
    	{
	      t->nice = INIT_THREAD_NICE;
	      t->recent_cpu = fix_int(INIT_RECENT_CPU);
	    }
      else
	    {
	      t->nice = thread_current ()->nice;
	      t->recent_cpu = thread_current ()->recent_cpu;
	    }
    }

  list_init (&t->children);
  list_init (&t->lock_list);
  t->blocking_lock = NULL;
  old_level = intr_disable ();
  if (thread_mlfqs)
    {
      recompute_priority_mlfqs (t, NULL);
    }

  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next, highest-priority thread to be scheduled.
   Should return a thread from one of the run queues, unless every run queue
   is empty.  (If the running thread can continue running, then it
   will be in a run queue.)  If all run queues are empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  int i;
  for (i = NUM_PRIO - 1; i >= 0; i--)
    {
      if (!list_empty(&ready_lists[i]))
        {
          return list_entry (list_pop_front (&ready_lists[i]),
            struct thread, elem);
        }
    }
  return idle_thread;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Inserts a thread into its corresponding ready list, based off of
 * its effective priority.
 */
static void
ready_lists_insert(struct thread *t)
{
  list_push_back(&ready_lists[t->eff_priority], &t->elem);
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Returns the thread whose tid matches the supplied tid, or NULL
   if no such thread exists. This function must be called with
   interrupts disabled. */
struct thread *
thread_lookup (tid_t tid)
{
  ASSERT (intr_get_level () == INTR_OFF);

  struct thread *t = NULL;
  struct list_elem *e;
  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      t = list_entry (e, struct thread, allelem);
      if (t->tid == tid)
        {
          break;
        }
    }
  return t;
}

struct child_state *
thread_child_lookup (struct thread *t, tid_t child_tid)
{
  struct list_elem *e;
  for (e = list_begin (&t->children); e != list_end (&t->children);
       e = list_next (e))
    {
      struct child_state *cs = list_entry (e, struct child_state, elem);
      if (cs->tid == child_tid)
        return cs;
    }
  return NULL;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
