#include <stdio.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "userprog/fdtable.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "lib/user/syscall.h"

/* A table mapping syscall numbers to the number of arguments
   their corresponding system calls take. */
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
static void sys_open (struct intr_frame *f, const char *path);
static void sys_filesize (struct intr_frame *f, int fd);
static void sys_read (struct intr_frame *f, int fd, void *buffer,
  unsigned length);
static void sys_write (struct intr_frame *f, int fd, const void *buffer,
  unsigned length);
static void sys_seek (struct intr_frame *f, int fd, unsigned position);
static void sys_tell (struct intr_frame *f, int fd);
static void sys_close (struct intr_frame *f, int fd);
static void sys_mkdir (struct intr_frame *f, const char *dir);

static bool is_valid_ptr (const void *ptr);
static bool is_valid_range (const void *ptr, size_t len);
static bool is_valid_string (const char *ptr);
static inline void exit_on (struct intr_frame *f, bool condition);
static inline void exit_on_file (struct intr_frame *f, bool condition);

/* A convenience function for exiting gracefully from
   errors in system calls. The supplied condition should be
   true iff some sort of bug occurred in the thread that
   requires it to exit with an exit code of -1. */
inline void
exit_on (struct intr_frame *f, bool condition)
{
  if (condition)
  {
    f->eax = -1;
    thread_current ()->exit_status = -1;
    thread_exit ();
  }
}

/* Similar to exit_on, but assumes that the filesystem
   lock is held before invoked. If the supplied condition
   is true, the lock is released and the thread is forced
   to exit as per exit_on. */
inline void
exit_on_file (struct intr_frame *f, bool condition)
{
  ASSERT (lock_held_by_current_thread (&filesys_lock));
  if (condition)
  {
    lock_release (&filesys_lock);
    exit_on (f, true);
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

  /* Extract the system call number and the arguments, if any. */
  exit_on (f, !is_valid_range (intr_esp, sizeof (uint32_t)));
  uint32_t syscall_num = *((uint32_t *)intr_esp);
  uint8_t arg_num = syscall_arg_num[syscall_num];

  int i;
  for (i = 0; i < arg_num; i++)
    {
      intr_esp = (uint32_t *)intr_esp + 1;
      exit_on (f, !is_valid_range (intr_esp, sizeof (uint32_t)));
      args[i] = *((uint32_t *)intr_esp);
    }

  /* Invoke the corresponding system call. */
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
        sys_close (f, (int)args[0]);
        break;
      case SYS_MMAP:
      case SYS_MUNMAP:
      case SYS_CHDIR:
      case SYS_MKDIR:
        sys_mkdir (f, (const char *)args[0]);
        break;
      case SYS_READDIR:
      case SYS_ISDIR:
      case SYS_INUMBER:
      default:
        exit_on (f, true); /* Unimplemented syscall --
                              force the thread to exit. */
    }
}

/* Returns true iff the suplied pointer points to a valid mapped
   user address. */
static bool
is_valid_ptr (const void *ptr)
{
  return (ptr != NULL) &&
    (is_user_vaddr(ptr)) &&
    (pagedir_get_page (thread_current ()->pagedir, ptr) != NULL);
}

/* Returns true iff every address within the range
   is a valid mapped user address. */
static bool
is_valid_range (const void *ptr, size_t len)
{
  size_t i;
  for (i = 0; i < len; i++)
    {
      if (!is_valid_ptr((uint8_t *)ptr + i))
        {
          return false;
        }
    }

  return true;
}

/* Returns true iff the supplied string spans
   valid mapped user memory. */
static bool
is_valid_string (const char *str)
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

static void
sys_halt (void)
{
  shutdown_power_off ();
}

static void
sys_exit (struct intr_frame *f, int status)
{
  f->eax = status;
  thread_current ()->exit_status = status;
  
  /* Communicate the exit status to this process' parent,
     if the parent exists. */
  enum intr_level old_level = intr_disable ();
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
  intr_set_level (old_level);

  thread_exit ();
}

static void
sys_exec (struct intr_frame *f, const char *file)
{
  exit_on (f, !is_valid_string(file));
  f->eax = process_execute (file);
}

static void
sys_wait (struct intr_frame *f, pid_t pid)
{
  f->eax = process_wait (pid);
}

static void
sys_create (struct intr_frame *f, const char *file,
  unsigned initial_size)
{
  exit_on (f, !is_valid_string (file));
  lock_acquire (&filesys_lock);
  f->eax = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
}

static void
sys_remove (struct intr_frame *f, const char *file)
{
  exit_on (f, !is_valid_string (file));
  lock_acquire (&filesys_lock);
  f->eax = filesys_remove (file);
  lock_release (&filesys_lock);
}

static void
sys_open (struct intr_frame *f, const char *path)
{
  exit_on (f, !is_valid_string (path));
  lock_acquire (&filesys_lock);
  f->eax = fd_table_open (path);
  lock_release (&filesys_lock);
}

static void
sys_filesize (struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);
  struct file *file = fd_table_get_file (fd);
  exit_on_file (f, file == NULL);
  f->eax = file_length (file);
  lock_release (&filesys_lock);
}

static void
sys_read (struct intr_frame *f, int fd, void *buffer,
  unsigned length)
{
  exit_on (f, fd == STDOUT_FILENO);
  exit_on (f, !is_valid_range (buffer, length));

  /* Read from STDIN if appropriate. */
  if (fd == STDIN_FILENO)
    {
      unsigned i;
      for (i = 0; i < length; i++)
        {
          ((uint8_t *)buffer)[i] = input_getc();
        }
      f->eax = length;
    }

  /* Otherwise, fetch the file and read the data in. */
  else
    {
      size_t tmp;
      size_t read_bytes = 0;
      lock_acquire (&filesys_lock);
      struct file *file = fd_table_get_file (fd);
      exit_on_file (f, file == NULL);
      while (read_bytes < length)
        {
          tmp = file_read (file, (uint8_t *)buffer + read_bytes,
            length - read_bytes);
          if (tmp == 0)
            {
              break;
            }
          read_bytes += tmp;
        }
      lock_release (&filesys_lock);
      f->eax = read_bytes;
    }
}


static void
sys_write (struct intr_frame *f, int fd, const void *buffer,
  unsigned length)
{
  exit_on (f, !is_valid_range (buffer, length));

  /* Read from STDOUT if appropriate. */
  if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, length);
      f->eax = length;
    }

  /* Ensure that an attempt to write a directory is not made. */
  else if (!fd_table_is_file (fd))
    {
      f->eax = -1;
    }

  /* Otherwise, fetch the file and write to it. */
  else
    {
      size_t tmp;
      size_t written_bytes = 0;
      lock_acquire (&filesys_lock);
      struct file *file = fd_table_get_file (fd);
      exit_on_file (f, file == NULL);
      while (written_bytes < length)
        {
          tmp = file_write (file, (uint8_t *)buffer + written_bytes,
            length - written_bytes);
          if (tmp == 0)
            {
              break;
            }
          written_bytes += tmp;
        }
      lock_release (&filesys_lock);
      f->eax = written_bytes;
    }
}

static void
sys_seek (struct intr_frame *f, int fd, unsigned position)
{
  lock_acquire (&filesys_lock);
  struct file *file = fd_table_get_file (fd);
  exit_on_file (f, file == NULL);
  file_seek (file, position);
  lock_release (&filesys_lock);
}

static void
sys_tell (struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);
  struct file *file = fd_table_get_file (fd);
  exit_on_file (f, file == NULL);
  f->eax = file_tell (file);
  lock_release (&filesys_lock);
}

static void
sys_close (struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);
  exit_on_file (f, !fd_table_close (fd));
  lock_release (&filesys_lock);
}

static void
sys_mkdir (struct intr_frame *f, const char *dir)
{
  exit_on (f, !is_valid_string (dir));
  lock_acquire (&filesys_lock);
  f->eax = filesys_mkdir (dir);
  lock_release (&filesys_lock);
}
