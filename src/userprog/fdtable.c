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

/* Open the supplied file, allocate a file descriptor for it,
   and return that file descriptor. Return -1 on error. */
int
fd_table_open (const char *file)
{
  struct thread *t = thread_current (); 
  struct file *f = filesys_open (file);
  if (f == NULL)
  {
    return -1;
  }

  /* Attempt to find an unused slot in our table for the
     newly opened file. */
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

  /* If we could not find an unused fd, then grow the table
     and select an fd. */
  if (!expand_table ())
    {
      return -1;
    }
  fd = t->fd_table_tail_idx + 1;
  t->fd_table_tail_idx++;
  t->fd_table[fd] = f;
  return fd;
}

/* Return a pointer to the struct file indexed by
   the supplied file descriptor, or NULL on error. */
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

/* Close the file corresponding to the supplied file
   descriptor and mark the file descriptor as unused.
   Return NULL on error. */
bool
fd_table_close (int fd)
{
  if (!fd_table_is_valid_fd (fd))
  {
    return false;
  }

  struct thread *t = thread_current ();
  file_close (t->fd_table[fd]);
  t->fd_table[fd] = NULL;

  if ((size_t)fd == t->fd_table_tail_idx)
    {
      t->fd_table_tail_idx--;
    }
  return true;
}

/* Return true iff the supplied file descriptor is valid.
   In particular, valid descriptors a) do not equal STDIN or
   STDOUT (this interface does not meaningfully interact with either
   of those descriptors), b) are less than the largest used descriptor,
   and c) are used. */
bool
fd_table_is_valid_fd (int fd)
{
  struct thread *t = thread_current ();
  return (fd != STDIN_FILENO) && (fd != STDOUT_FILENO) &&
    ((size_t)fd <= t->fd_table_tail_idx) && (t->fd_table[fd] != NULL);
}

/* Return an unused fd, or -1 if no such fd was found. */
static int
find_unused_fd (void)
{
  struct thread *t = thread_current ();

  /* First, attempt to use the highest unused fd. */
  if (t->fd_table_tail_idx < (t->fd_table_size - 1))
    {
      return t->fd_table_tail_idx + 1;
    }

  /* Comb through the table and look for an unused fd. */
  size_t i;
  for (i = 2; i < t->fd_table_tail_idx; i++)
    {
      if (t->fd_table[i] == NULL)
        {
          return i;
        }
    }
  return -1;
}

/* Grow the file descriptor table by a factor of FD_EXPAND_FACTOR;
   return false on error, true otherwise. */
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
