#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "userprog/fdtable.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "filesys/directory.h"

/* Expand by a constant factor each time. */
#define FD_EXPAND_FACTOR 2

enum fd_type
  {
    FD_DIRECTORY,
    FD_FILE,
  };

struct fd_entry
  {
    enum fd_type fd_type;
    union
      {
        struct file *file;
        struct dir *dir;
      };
  };

static int find_unused_fd (void);
static bool expand_table (void);

// Resolve the pathname
// Determine if we're opening a directory or a file.
// We do this by looking at entry_name -- if, when doing a lookup,
// we find that entry_name is a dir, then it's a dir; otherwise, it's
// a file.
// We can have a nice filesys function that does this and tells us
// if a path is a dir or a file.

/* Open the supplied file, allocate a file descriptor for it,
   and return that file descriptor. Return -1 on error. */
int
fd_table_open (const char *path)
{
  struct file *f;
  struct dir *d;
  if (!filesys_open_generic (path, &f, &d))
    {
      return -1;
    }
  /* TODO: Remove assert. */
  ASSERT (f != NULL || d != NULL);

  struct fd_entry *fd_entry = malloc (sizeof fd_entry);
  if (f)
    {
      fd_entry->fd_type = FD_FILE;
      fd_entry->file = f;
    }
  else
    {
      fd_entry->fd_type = FD_DIRECTORY;
      fd_entry->dir = d;
    }

  /* Attempt to find an unused slot in our table for the
     newly opened file. */
  struct thread *t = thread_current ();
  int fd = find_unused_fd ();
  if (fd > 0)
    {
      t->fd_table[fd] = fd_entry;
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
      free (fd_entry);
      if (f)
        {
          file_close (f);
        }
      else
        {
          dir_close (d);
        }
      return -1;
    }

  fd = t->fd_table_tail_idx + 1;
  t->fd_table_tail_idx++;
  t->fd_table[fd] = fd_entry;
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
  return t->fd_table[fd]->file;
}

/* Return a pointer to the struct dir indexed by
   the supplied file descriptor, or NULL on error. */
struct dir *
fd_table_get_dir (int fd)
{
  if (!fd_table_is_valid_fd (fd))
  {
    return NULL;
  }
  struct thread *t = thread_current ();
  return t->fd_table[fd]->dir;
}

/* Close the file or directory corresponding to the supplied file
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
  switch (t->fd_table[fd]->fd_type)
    {
      case FD_FILE:
        file_close (t->fd_table[fd]->file);
        break;
      case FD_DIRECTORY:
        dir_close (t->fd_table[fd]->dir);
        break;
      default:
        NOT_REACHED ();
    }
  free (t->fd_table[fd]);
  t->fd_table[fd] = NULL;

  if ((size_t)fd == t->fd_table_tail_idx)
    {
      t->fd_table_tail_idx--;
    }
  return true;
}

/* Returns the inumber of the fd, or -1 on error. */
int fd_table_inumber (int fd)
{
  if (!fd_table_is_valid_fd (fd))
    {
      return false;
    }

  struct thread *t = thread_current ();
  if (t->fd_table[fd]->fd_type == FD_FILE)
    {
      return inode_get_inumber (file_get_inode (t->fd_table[fd]->file));
    }
  else
    {
      return inode_get_inumber (dir_get_inode (t->fd_table[fd]->dir));
    }
}

/* Returns true iff a file is indexed by fd. */
bool fd_table_is_file (int fd)
{
  if (!fd_table_is_valid_fd (fd))
    {
      return false;
    }

  return thread_current ()->fd_table[fd]->fd_type == FD_FILE;
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

/* Closes every file and directory in the fd_table, but
   does not free the fd_table itself. */
void
fd_table_dispose (void)
{
  struct thread *t = thread_current ();

  size_t i;
  for (i = 0; i <= t->fd_table_tail_idx; i++)
    {
      if (t->fd_table[i] != NULL)
      {
        fd_table_close (i);
      }
    }
}

/* Grow the file descriptor table by a factor of FD_EXPAND_FACTOR;
   return false on error, true otherwise. */
static bool
expand_table (void)
{
  struct thread *t = thread_current ();

  size_t new_size = t->fd_table_size * FD_EXPAND_FACTOR;
  struct fd_entry **new_fd_table = calloc (new_size,
    sizeof (struct fd_entry *));
  if (new_fd_table == NULL)
    {
      return false;
    }

  memcpy (new_fd_table, t->fd_table,
    t->fd_table_size * sizeof (struct fd_entry *));
  free (t->fd_table);
  t->fd_table = new_fd_table;
  t->fd_table_size = new_size;
  return true;
}
