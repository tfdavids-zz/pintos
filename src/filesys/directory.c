#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/interrupt.h"

#define PATH_DELIM '/'

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry), I_DIR);
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
/* TODO: Confirm inode is a dir? */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open_inumber (ROOT_DIR_SECTOR);
}

/* Opens the directory corresponding to the supplied
   inumber. Returns true if successful, false on failure. */
struct dir *
dir_open_inumber (block_sector_t sector)
{
  return dir_open (inode_open (sector));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* TODO: This should use the block cache ... */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/* Resolves a path, storing the bottom-most directiroy in *dir and the
   file or directory name in the name buffer. Returns true if and only if the
   resolution was successful.

   Precondition: name[] must be at least NAME_MAX + 1 bytes long.
   NB: Success does not necessarily imply that a file or directory with
       name 'name' exists in the outputted directory. dir_lookup should be
       invoked with parameters dir and name in order to check whether name
       really does exist in dir.
   NB: On success, it is the user's responsibility to close *dir. */
bool
dir_resolve_path (const char *path, struct dir **dir, char name[])
{
  /* Sanity checks. */
  if (path == NULL || *path == '\0')
    {
      return false;
    }

  /* Make a copy of the user string, for convenience. */
  size_t len = strlen (path);
  char *path_cpy = malloc (len + 1);
  strlcpy (path_cpy, path, len + 1);

  /* Strip trailing slashes, if any. */
  size_t i = len - 1;
  for (i = len - 1; i > 0 && path_cpy[i] == PATH_DELIM; i--, len--)
    {
      path_cpy[i] = '\0';
    }

  /* Determine which directory we will begin in. */
  struct dir *curr_dir;
  char *left = path_cpy;
  if (path_cpy[0] == PATH_DELIM)
    {
      curr_dir = dir_open_root ();
      for (; *left == PATH_DELIM; left++); /* Skip leading slashes. */
    }
  else
    {
      if (thread_current ()->working_dir_inumber == 0)
        {
          free (path_cpy);
          return false;
        }
      curr_dir = dir_open_inumber (thread_current ()->working_dir_inumber);
    }

  /* Iteratively resolve the pathname. */
  struct dir_entry curr_dir_ent;
  char curr_name[NAME_MAX + 1];
  char *right;
  while ((right = strchr (left, PATH_DELIM)) != NULL)
    {
      /* Lift the name of the directory. */
      if (right - left > NAME_MAX)
        {
          free (path_cpy);
          return false;
        }
      strlcpy (curr_name, left, right - left + 1);

      /* Confirm that this is a directory. */
      /* TODO */

      /* Do a lookup for the entry. */
      if (!lookup (curr_dir, curr_name, &curr_dir_ent, NULL))
        {
          free (path_cpy);
          return false;
        }

      /* close old dir, open new dir, advance left. */
      dir_close (curr_dir);
      curr_dir = dir_open_inumber (curr_dir_ent.inode_sector);
      if (curr_dir == NULL)
        {
          free (path_cpy);
          return false;
        }
      for (left = right; *left == PATH_DELIM; left++);
    }

  if (len - (uintptr_t)(left - path_cpy) > NAME_MAX)
    {
      free (path_cpy);
      return false;
    }
  strlcpy (name, left, NAME_MAX + 1);
  *dir = curr_dir;
  free (path_cpy);
  return true;
}

/* Adds an entry named NAME to DIR, which must not already contain a
   file by that name.  The entry's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Ensure that the directory has not been deleted. */
  if (inode_is_removed (dir->inode))
    return false;

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  /* TODO: Block cache */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  /* TODO: Block cache */
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

static void dir_invalidate (struct thread *t, void *aux)
{
  block_sector_t sector = *((block_sector_t *)aux);
  if (t->working_dir_inumber == sector)
    {
      t->working_dir_inumber = 0;
    }
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* If inode is a directory, ensure that it is empty before
     removing it. */
  struct dir *dir_rm = NULL;
  if (!inode_is_file (inode))
    {
      dir_rm = dir_open (inode);
      struct dir_entry curr;
      off_t pos;
      for (pos = 0; inode_read_at (dir_rm->inode, &curr, sizeof curr, pos) ==
        sizeof curr; pos += sizeof curr)
        {
          if (curr.in_use && strcmp(curr.name, CURR_DIR) != 0 &&
            strcmp (curr.name, PREV_DIR) != 0)
            {
              goto done;
            }
        }

      /* Remove . and .. */
      struct dir_entry curr_dir, prev_dir;
      off_t curr_dir_ofs, prev_dir_ofs;
      if (!lookup (dir_rm, CURR_DIR, &curr_dir, &curr_dir_ofs) ||
          !lookup (dir_rm, PREV_DIR, &prev_dir, &prev_dir_ofs))
        {
          goto done;
        }
      curr_dir.in_use = prev_dir.in_use = false;
      if ((inode_write_at (dir_rm->inode, &curr_dir, sizeof curr_dir,
                          curr_dir_ofs) != sizeof curr_dir) ||
          (inode_write_at (dir_rm->inode, &prev_dir, sizeof prev_dir,
                          prev_dir_ofs) != sizeof prev_dir))
        {
          goto done;
        }

      block_sector_t invalid = inode_get_inumber (dir_rm->inode);
      intr_disable ();
      thread_foreach (dir_invalidate, &invalid);
      intr_enable ();
    }

  /* Erase directory entry. */
  e.in_use = false;
  /* TODO: Block cache */
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  if (dir != NULL)
    {
      dir_close (dir_rm);
    }
  else
    {
      inode_close (inode);
    }
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use && strcmp(e.name, CURR_DIR) != 0 &&
        strcmp (e.name, PREV_DIR) != 0)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          printf ("Found entry! %s\n", name);
          return true;
        } 
    }
  return false;
}
