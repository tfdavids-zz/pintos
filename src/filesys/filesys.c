#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  block_sector_t inode_sector = 0;
  struct dir *dir;
  char entry_name[NAME_MAX + 1];
  if (!dir_resolve_path (name, &dir, entry_name))
    {
      return false;
    }

  /* TODO: Distinguish between creating files and creating directories! */
  /* TODO: If creating a directory, then add "." and ".." dir_entries to
           the directory! */
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, entry_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  /* TODO Resolve pathname. */
  /* TODO Resolve directory implicitly specified by pathname. */
  struct dir *dir;
  char entry_name[NAME_MAX + 1];
  if (!dir_resolve_path (name, &dir, entry_name))
    {
      return false;
    }
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, entry_name, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* TODO: filesys_open_dir */

/* TODO: Need a filesys_remove_dir fn. Or, need this
   to work with directory names as well. */
/* TODO: Ensure that open (and therefore working as well)
   directories cannot be removed. (Maybe) */

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  /* TODO Resolve pathname. */
  /* TODO Resolve directory implicitly specified by pathname. */
  struct dir *dir;
  char entry_name[NAME_MAX + 1];
  if (!dir_resolve_path (name, &dir, entry_name))
    {
      return false;
    }
  bool success = dir != NULL && dir_remove (dir, name);
  dir_close (dir); 

  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
