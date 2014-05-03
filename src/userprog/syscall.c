#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "lib/user/syscall.h"
#include "devices/shutdown.h"

/* TODO: Is it appropriate for the kernel to include header files designed
 * for user-land? */

#define MAX_ARGS 3
static uint8_t syscall_arg_num[] =
  {0, 1, 1, 1, 2, 1, 1, 1, 3, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1};

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *f);
static void sys_halt (void) NO_RETURN;
static void sys_exit (struct intr_frame *f, int status) NO_RETURN;
static void sys_exec (struct intr_frame *f, const char *file);
static void sys_wait (struct intr_frame *f, pid_t pid);
static void sys_create (struct intr_frame *f, const char *file,
  unsigned initial_size);
static void sys_remove (struct intr_frame *f, const char *file);
static void sys_open (struct intr_frame *f, const char *file);
static void sys_filesize (struct intr_frame *f, int fd);
static void sys_read (struct intr_frame *f, int fd, void *buffer,
  unsigned length);
static void sys_write (struct intr_frame *f, int fd, const void *buffer,
  unsigned length);
static void sys_seek (struct intr_frame *f, int fd, unsigned position);
static void sys_tell (struct intr_frame *f, int fd);
static void sys_close (struct intr_frame *f, int fd);
static bool is_valid_ptr (const void *ptr);
static bool is_valid_range (const void *ptr, size_t len);
static bool is_valid_string (const char *ptr);
static inline void bug_on (struct intr_frame *f, bool condition);

static inline void bug_on (struct intr_frame *f, bool condition)
{
  if (condition)
  {
    f->eax = -1;
    thread_current ()->exit_status = -1;
    thread_exit ();
  }
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t args[MAX_ARGS];
  void *intr_esp = f->esp;

  bug_on (f, !is_valid_range (intr_esp, sizeof (uint32_t)));
  uint32_t syscall_num = *((uint32_t *)intr_esp);
  uint8_t arg_num = syscall_arg_num[syscall_num];
  
  int i;
  for (i = 0; i < arg_num; i++)
    {
      intr_esp = (uint32_t *)intr_esp + 1;
      bug_on (f, !is_valid_range (intr_esp, sizeof (uint32_t)));
      args[i] = *((uint32_t *)intr_esp);
    }

  switch (syscall_num)
    {
      case SYS_HALT:
        sys_halt ();
        break;
      case SYS_EXIT:
        sys_exit (f, (int)args[0]);
        break;
      case SYS_EXEC:
        sys_exec (f, (const char *)args[0]);
        break;
      case SYS_WAIT:
        sys_wait (f, (pid_t)args[0]);
        break;
      case SYS_CREATE:
        sys_create (f, (const char *)args[0], (unsigned)args[1]);
        break;
      case SYS_REMOVE:
        sys_remove (f, (const char *)args[0]);
        break;
      case SYS_OPEN:
        sys_open (f, (const char *)args[0]);
        break;
      case SYS_FILESIZE:
        sys_filesize (f, (int)args[0]);
        break;
      case SYS_READ:
        sys_read (f, (int)args[0], (void *)args[1], (unsigned)args[2]);
        break; 
      case SYS_WRITE:
        sys_write (f, (int)args[0], (const void *)args[1], (unsigned)args[2]);
        break;
      case SYS_SEEK:
        sys_seek (f, (int)args[0], (unsigned)args[1]);
        break;
      case SYS_TELL:
        sys_tell (f, (int)args[0]);
        break;
      case SYS_CLOSE:
      case SYS_MMAP:
      case SYS_MUNMAP:
      case SYS_CHDIR:
      case SYS_MKDIR:
      case SYS_READDIR:
      case SYS_ISDIR:
      case SYS_INUMBER:
      default:
        /* TODO: Decide what to do in this case. */
        printf ("System call not implemented.\n");
    }
}

static bool is_valid_ptr (const void *ptr)
{
  return (ptr != NULL) &&
    (is_user_vaddr(ptr)) &&
    (pagedir_get_page (thread_current ()->pagedir, ptr) != NULL);
}

static bool is_valid_range (const void *ptr, size_t len)
{
  size_t i;
  for (i = 0; i < len; i++)
    {
      if (!is_valid_ptr((uint8_t *)ptr + i))
        return false;
    }

  return true;
}

static bool is_valid_string (const char *str)
{
  if (!is_valid_ptr (str))
    {
      return false;
    }

  while (*str != '\0')
    {
      str++;
      if (!is_valid_ptr(str))
        {
          return false;
        }
    }
  return true;
}

/* TODO: Validate user memory. */
static void sys_halt (void)
{
  shutdown_power_off ();
}

static void sys_exit (struct intr_frame *f, int status)
{
  f->eax = status;
  thread_current ()->exit_status = status;
  struct thread *parent = thread_lookup (thread_current ()->parent_tid);
  if (parent)
    {
      struct child_state *cs = thread_child_lookup(parent,
						   thread_current ()->tid);
      if (cs)
        {
          cs->exit_status = status;
        }
    }

  thread_exit ();
}

static void sys_exec (struct intr_frame *f, const char *file)
{
  bug_on (f, !is_valid_string(file));
  int tid = process_execute (file);
  f->eax = tid;
}

static void sys_wait (struct intr_frame *f, pid_t pid)
{
  f->eax = process_wait (pid);
}

/* TODO: access_ok */
static void sys_create (struct intr_frame *f, const char *file,
  unsigned initial_size)
{
  bug_on (f, !is_valid_string (file));
  lock_acquire (&filesys_lock);
  f->eax = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
}

/* TODO: access_ok */
static void sys_remove (struct intr_frame *f, const char *file)
{
  ASSERT(false);
}

/* TODO: access_ok */
static void sys_open (struct intr_frame *f, const char *file)
{
  ASSERT(false);
}

static void sys_filesize (struct intr_frame *f, int fd)
{
  ASSERT(false);
}

static void sys_read (struct intr_frame *f, int fd, void *buffer,
  unsigned length)
{
  bug_on (f, fd == STDOUT_FILENO);
  /* TODO: Remove assert */
  ASSERT (fd == STDIN_FILENO);
  bug_on (f, !is_valid_range (buffer, length));
  int i;
  for (i = 0; i < length; i++)
    {
      ((uint8_t *)buffer)[i] = input_getc();
    }
  return length; /* TODO: return number of bytes actually read! */
  
}


static void sys_write (struct intr_frame *f, int fd, const void *buffer,
  unsigned length)
{
  /* TODO: Remove assert */
  ASSERT (fd == STDOUT_FILENO);
  bug_on (f, !is_valid_range (buffer, length));
  putbuf (buffer, length);
  f->eax = length; // TODO: return number of bytes actually written!
}

static void sys_seek (struct intr_frame *f, int fd, unsigned position)
{
  ASSERT(false);
}

static void sys_tell (struct intr_frame *f, int fd)
{
  ASSERT(false);
}

static void sys_close (struct intr_frame *f, int fd)
{
  ASSERT(false);
}
