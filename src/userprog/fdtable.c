#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "userprog/fdtable.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

/* Expand by a constant factor each time. */
#define FD_EXPAND_FACTOR 2

static int find_unused_fd (void);
static bool expand_table (void);

int
fd_table_open (const char *file)
{
  /* assume file is valid */
  /* try using fd equal to tail_idx + 1.
   * if cannot, then try finding unused fd from 2 to tail_idx */
  /* if still cannot, expand table and then use tail_idx + 1. */
  /* TODO: figure out if a file is already open */
  struct thread *t = thread_current (); 
  struct file *f = filesys_open (file);
  if (f == NULL)
  {
    return -1;
  }

  /* try to find an unused slot */
  int fd = find_unused_fd ();
  if (fd > 0)
    {
      t->fd_table[fd] = f;
      if ((size_t )fd > t->fd_table_tail_idx)
        {
          t->fd_table_tail_idx = fd;
        }
      return fd;
    }

  /* expand the table and return the next slot. */
  if (!expand_table ())
    {
      return -1;
    }
  fd = t->fd_table_tail_idx + 1;
  t->fd_table_tail_idx++;
  t->fd_table[fd] = f;
  return fd;
}

struct file *
fd_table_get_file (int fd)
{
  if (!fd_table_is_valid_fd (fd))
  {
    return NULL;
  }
  struct thread *t = thread_current ();
  return t->fd_table[fd];
}

bool
fd_table_close (int fd)
{
  /*  Make sure fd is valid */
  if (!fd_table_is_valid_fd (fd))
  {
    return false;
  }

  /*  given that fd is valid, clear its entry and free the memory. */
  struct thread *t = thread_current ();

  file_close (t->fd_table[fd]);
  t->fd_table[fd] = NULL;

  if ((size_t)fd == t->fd_table_tail_idx)
    {
      t->fd_table_tail_idx--;
    }
  return true;
}

bool
fd_table_is_valid_fd (int fd)
{
  /* fd cannot be 0 or 1 */
  /* fd must not be greater than fd_table_tail_idx */
  /* fd must be used */
  struct thread *t = thread_current ();
  return (fd != STDIN_FILENO) && (fd != STDOUT_FILENO) &&
    ((size_t)fd <= t->fd_table_tail_idx) && (t->fd_table[fd] != NULL);
}

static int
find_unused_fd (void)
{
  struct thread *t = thread_current ();
  if (t->fd_table_tail_idx < (t->fd_table_size - 1))
    {
      return t->fd_table_tail_idx + 1;
    }

  /* Comb through the table and look for unused */
  size_t i;
  for (i = 2; i < t->fd_table_tail_idx; i++)
    {
      if (t->fd_table[i] == NULL)
        {
          return i;
        }
    }

  /* Could not find an unused fd. */
  return -1;
}

static bool
expand_table (void)
{
  struct thread *t = thread_current ();

  size_t new_size = t->fd_table_size * FD_EXPAND_FACTOR;
  struct file **new_fd_table = calloc (new_size, sizeof (struct file *));
  if (new_fd_table == NULL)
    {
      return false;
    }

  memcpy (new_fd_table, t->fd_table,
    t->fd_table_size * sizeof (struct file *));
  free (t->fd_table);
  t->fd_table = new_fd_table;
  t->fd_table_size = new_size;
  return true;
}
