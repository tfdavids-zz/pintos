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
#include "vm/page.h"

/* A table mapping syscall numbers to the number of arguments
   their corresponding system calls take. */
#define MAX_ARGS 3
static uint8_t syscall_arg_num[] =
  {0, 1, 1, 1, 2, 1, 1, 1, 3, 3, 2, 1, 1, 2, 1, 1, 1, 2, 1, 1};

static void syscall_handler (struct intr_frame *f);
static void sys_halt (void) NO_RETURN;
static void sys_exit (int status) NO_RETURN;
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
static void sys_seek (int fd, unsigned position);
static void sys_tell (struct intr_frame *f, int fd);
static void sys_close (int fd);
static void sys_mmap (struct intr_frame *f, int fd, void *addr);
static void sys_munmap (mapid_t mapping);

static bool is_valid_ptr (const void *ptr, struct intr_frame *f);
static bool is_valid_range (const void *ptr, size_t len, struct intr_frame *f);
static bool is_valid_string (const char *ptr, struct intr_frame *f);
static bool unpin_ptr (const void *ptr);
static bool unpin_range (const void *ptr, size_t len);
static bool unpin_string (const char *ptr);
static inline void exit_on (bool condition);
static inline void exit_on_file (bool condition);

/* A convenience function for exiting gracefully from
   errors in system calls. The supplied condition should be
   true iff some sort of bug occurred in the thread that
   requires it to exit with an exit code of -1. */
inline void
exit_on (bool condition)
{
  if (condition)
  {
    sys_exit (-1);
  }
}

/* Similar to exit_on, but assumes that the filesystem
   lock is held before invoked. If the supplied condition
   is true, the lock is released and the thread is forced
   to exit as per exit_on. */
inline void
exit_on_file (bool condition)
{
  ASSERT (lock_held_by_current_thread (&filesys_lock));
  if (condition)
  {
    lock_release (&filesys_lock);
    exit_on (true);
  }
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  uint32_t args[MAX_ARGS];
  void *intr_esp = f->esp;

  /* Extract the system call number and the arguments, if any. */
  exit_on (!is_valid_range (intr_esp, sizeof (uint32_t), f));
  uint32_t syscall_num = *((uint32_t *)intr_esp);
  uint8_t arg_num = syscall_arg_num[syscall_num];

  int i;
  for (i = 0; i < arg_num; i++)
    {
      intr_esp = (uint32_t *)intr_esp + 1;
      exit_on (!is_valid_range (intr_esp, sizeof (uint32_t), f));
      args[i] = *((uint32_t *)intr_esp);
    }

  /* Invoke the corresponding system call. */
  switch (syscall_num)
    {
      case SYS_HALT:
        sys_halt ();
        break;
      case SYS_EXIT:
        sys_exit ((int)args[0]);
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
        sys_seek ((int)args[0], (unsigned)args[1]);
        break;
      case SYS_TELL:
        sys_tell (f, (int)args[0]);
        break;
      case SYS_CLOSE:
        sys_close ((int)args[0]);
        break;
      case SYS_MMAP:
        sys_mmap (f, (int)args[0], (void *)args[1]);
        break;
      case SYS_MUNMAP:
        sys_munmap ((mapid_t)args[0]);
        break;
      case SYS_CHDIR:
      case SYS_MKDIR:
      case SYS_READDIR:
      case SYS_ISDIR:
      case SYS_INUMBER:
      default:
        exit_on (true); /* Unimplemented syscall --
                              force the thread to exit. */
    }
  /* Unpin syscall arguments */
  intr_esp = f->esp;
  for (i = 0; i <= arg_num; i++)
    {
      unpin_range (intr_esp, sizeof (uint32_t));
      intr_esp = (uint32_t *)intr_esp + 1;
    }
}

/* Returns true iff the suplied pointer points to a valid mapped
   user address. */
static bool
is_valid_ptr (const void *ptr, struct intr_frame *f)
{
  /* TODO: supp_pte can exist but it's possible that ptr is not mapped.
     this could cause a page fault when ptr is later accessed. */
  struct supp_pte *e;

  if ((ptr == NULL) || !is_user_vaddr (ptr))
    {
      return false;
    }
  if (ptr >= f->esp - 32 && ptr > STACK_LIMIT)
    {
      if (supp_pt_lookup (&thread_current ()->supp_pt,
                          pg_round_down (ptr)) == NULL)
        {
          if (!supp_pt_page_calloc (&thread_current ()->supp_pt,
                                   pg_round_down (ptr), true))
            {
              thread_current ()->exit_status = -1;
              thread_exit ();
            }
        }
    }

  if ((e = supp_pt_lookup (&thread_current ()->supp_pt, ptr)) == NULL)
    {
      return false;
    }

  /* Pin pages, and keep them pinned untill done with the system call */
  e->pinned = true;

    /* stack growth TODO: Max stack addr */

  return page_force_load (e);
}

/* Returns true iff every address within the range
   is a valid mapped user address. */
static bool
is_valid_range (const void *ptr, size_t len,  struct intr_frame *f)
{
  void* curr_page = pg_round_down (ptr);
  size_t i;
  while (curr_page < (uint8_t *)ptr + len)
    {
      if (!is_valid_ptr (curr_page, f))
        {
          return false;
        }
      curr_page = (uint8_t *)curr_page + PGSIZE;
    }

  return true;
}

/* Returns true iff the supplied string spans
   valid mapped user memory. */
static bool
is_valid_string (const char *str,  struct intr_frame *f)
{
  if (!is_valid_ptr (str, f))
    {
      return false;
    }

  while (*str != '\0')
    {
      str++;
      if (pg_round_down (str) == str)
        {
          if (!is_valid_ptr(str, f))
            {
              return false;
            }
        }
    }

  return true;
}

/* Finds frame for given ptr and unpins it */
static bool
unpin_ptr (const void *ptr)
{
  struct supp_pte *e;

  if ((e = supp_pt_lookup (&thread_current ()->supp_pt, ptr)) == NULL)
    {
      return false;
    }

  /* Pin pages, and keep them pinned untill done with the system call */
  e->pinned = false;
  return true;
}

/* Unpins all ptrs in given range. */
static bool
unpin_range (const void *ptr, size_t len)
{
  void* curr_page = pg_round_down (ptr);

  while (curr_page < (uint8_t *)ptr + len)
    {
      if (!unpin_ptr (curr_page))
        {
          return false;
        }
      curr_page = (uint8_t *)curr_page + PGSIZE;
    }
  return true;
}

/* Unpins all ptrs in given string. */
static bool
unpin_string (const char *str)
{
  if (!unpin_ptr (str))
    {
      return false;
    }

  while (*str != '\0')
    {
      str++;
      if (pg_round_down (str) == str)
        {
          if (!unpin_ptr(str))
            {
              return false;
            }
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
sys_exit (int status)
{
  thread_current ()->exit_status = status;
  thread_exit ();
}

static void
sys_exec (struct intr_frame *f, const char *file)
{
  exit_on (!is_valid_string(file, f));
  f->eax = process_execute (file);
  unpin_string (file);
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
  exit_on (!is_valid_string (file, f));
  lock_acquire (&filesys_lock);
  f->eax = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  unpin_string (file);
}

static void
sys_remove (struct intr_frame *f, const char *file)
{
  exit_on (!is_valid_string (file, f));
  lock_acquire (&filesys_lock);
  f->eax = filesys_remove (file);
  lock_release (&filesys_lock);
  unpin_string (file);
}

static void
sys_open (struct intr_frame *f, const char *file)
{
  exit_on (!is_valid_string (file, f));
  lock_acquire (&filesys_lock);
  f->eax = fd_table_open (file);
  lock_release (&filesys_lock);
  unpin_string (file);
}

static void
sys_filesize (struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);
  struct file *file = fd_table_get_file (fd);
  exit_on_file (file == NULL);
  f->eax = file_length (file);
  lock_release (&filesys_lock);
}

static void
sys_read (struct intr_frame *f, int fd, void *buffer,
  unsigned length)
{
  exit_on (fd == STDOUT_FILENO);
  exit_on (!is_valid_range (buffer, length, f));

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
      exit_on_file (file == NULL);
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
  unpin_range (buffer, length);
}


static void
sys_write (struct intr_frame *f, int fd, const void *buffer,
  unsigned length)
{
  exit_on (!is_valid_range (buffer, length, f));

  /* Read from STDOUT if appropriate. */
  if (fd == STDOUT_FILENO)
    {
      putbuf (buffer, length);
      f->eax = length;
    }

  /* Otherwise, fetch the file and write to it. */
  else
    {
      size_t tmp;
      size_t written_bytes = 0;
      lock_acquire (&filesys_lock);
      struct file *file = fd_table_get_file (fd);
      exit_on_file (file == NULL);
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
  unpin_range (buffer, length);
}

static void
sys_seek (int fd, unsigned position)
{
  lock_acquire (&filesys_lock);
  struct file *file = fd_table_get_file (fd);
  exit_on_file (file == NULL);
  file_seek (file, position);
  lock_release (&filesys_lock);
}

static void
sys_tell (struct intr_frame *f, int fd)
{
  lock_acquire (&filesys_lock);
  struct file *file = fd_table_get_file (fd);
  exit_on_file (file == NULL);
  f->eax = file_tell (file);
  lock_release (&filesys_lock);
}

static void
sys_close (int fd)
{
  lock_acquire (&filesys_lock);
  exit_on_file (!fd_table_close (fd));
  lock_release (&filesys_lock);
}

static void
sys_mmap (struct intr_frame *f, int fd, void *addr)
{
  int length;
  size_t num_pages;
  size_t i;
  size_t bytes;
  mapid_t mapid;
  void *curr_page;
  struct thread *t = thread_current ();

  /* Ensure that the fd is valid. */
  struct file *file = fd_table_get_file (fd);
  if (file == NULL)
    {
      f->eax = MAP_FAILED;
      return;
    }

  /* Ensure that the file length is positive. */
  lock_acquire (&filesys_lock);
  length = file_length (file);
  lock_release (&filesys_lock);
  if (length <= 0)
    {
      f->eax = MAP_FAILED;
      return;
    }

  /* Reopen the file for this process. */
  file = file_reopen (file);

  /* Ensure that addr is acceptable. */
  if (addr == NULL || pg_ofs (addr) != 0 || !is_user_vaddr (addr))
    {
      f->eax = MAP_FAILED;
      return; 
    }

  /* Ensure that there is enough space for the addr. */
  /* Calculate each page and make sure that none is mapped. */
  /* TODO: Ensure that the pages do not overlap with other
           segments (e.g. the stack). */
  /* TODO: Synchronization (what if someone creates a page
           after we've determined it does not exist?). */
  num_pages = pg_range_num(length);
  for (i = 0; i < num_pages; i++)
    {
      if (supp_pt_page_exists (&t->supp_pt, addr + i * PGSIZE))
        {
          f->eax = MAP_FAILED;
          return;
        }
    }

    /* Error checking passes. We can mmap it. */
    /* TODO: This is a hack, but a very convenient one:
       Simply choose mapid equal to (mapid_t) addr.
       Assumes that the size of mapid_t matches the size
       of pointers. */
    /* TODO: File permissions? They will be exercised
             when the memory is unmapped and written back
             to the file, but it would be nice to preemptively
             set page permissions here. */
    mapid = (mapid_t) addr;
    curr_page = addr;
    for (i = 0; i < num_pages; i++)
      {
        bytes = length > PGSIZE ? PGSIZE : length;

        /* TODO: Debugging. */
        ASSERT (bytes > 0);

        if (!supp_pt_page_alloc_file (&t->supp_pt, curr_page,
          file, i * PGSIZE, bytes, mapid, true))
          {
            /* Allocation failed. */
            /* TODO: Clean up the pages we just allocated. */
            while (i > 0)
              {
                curr_page = (void *)((uintptr_t)curr_page - PGSIZE);
                supp_pt_page_free (&t->supp_pt, curr_page);
                i--;
              }
            f->eax = MAP_FAILED;
            return;
          }

        length -= bytes;
        curr_page = (void *)((uintptr_t)curr_page + PGSIZE);
      }

    /* TODO: Debugging. */
   // ASSERT (bytes == 0);
    f->eax = mapid;
}

static void
sys_munmap (mapid_t mapping)
{
  exit_on (!supp_pt_munmap (&thread_current ()->supp_pt, (void *)mapping));
}
