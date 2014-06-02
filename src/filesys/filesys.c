#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/free-map.h"
#include "filesys/inode.h"

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
  bool success = (free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, I_FILE)
                  && dir_add (dir, entry_name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

bool
filesys_mkdir (const char *name)
{
  block_sector_t inode_sector = 0;
  struct dir *dir;
  char entry_name[NAME_MAX + 1];
  if (!dir_resolve_path (name, &dir, entry_name))
    {
      return false;
    }

  /* TODO: Directory size? */
  bool success = (free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, 16)
                  && dir_add (dir, entry_name, inode_sector));
  if (!success && inode_sector != 0) 
    {
      goto done;
    }

  /* Add entries for '.' and '..' to the directory. 
     TODO: Verify that dir_create / dir_add fail if try to create
           root dir (i.e. entry_name == 0, dir == root dir) */
  struct dir *new_dir = dir_open_inumber (inode_sector);
  success = (dir_add (new_dir, CURR_DIR,
            inode_get_inumber (dir_get_inode (new_dir)))
            && dir_add (new_dir, PREV_DIR,
            inode_get_inumber (dir_get_inode (new_dir))));
    
  if (!success)
    {
      dir_close (new_dir);
    }
  
 done:
  dir_close (dir);
  return success;
}


/* Opens a path, which might be a file or a directory. */
bool
filesys_open_generic (const char *name, struct file **file, struct dir **dir)
{
  *file = *dir = NULL;

  struct dir *bottom_dir;
  char entry_name[NAME_MAX + 1];
  if (!dir_resolve_path (name, &bottom_dir, entry_name))
    {
      return false;
    }
  struct inode *inode = NULL;

  if (strlen (entry_name) == 0)
    {
      /* TODO: Remove assert */
      ASSERT (inode_get_inumber (dir_get_inode (bottom_dir)) ==
                                 ROOT_DIR_SECTOR);
      *dir = bottom_dir;
      return true;
    }

  dir_lookup (bottom_dir, entry_name, &inode);
  if (inode == NULL)
    {
      dir_close (bottom_dir);
      return false;
    }

  if (inode_is_file (inode))
    {
      *file = file_open (inode);
      dir_close (bottom_dir);
      return *file != NULL;
    }
  else
    {
      *dir = dir_open (inode);
      dir_close (bottom_dir);
      return *dir != NULL;
    }
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir *dir;
  char entry_name[NAME_MAX + 1];
  if (!dir_resolve_path (name, &dir, entry_name))
    {
      return false;
    }
  struct inode *inode = NULL;

  dir_lookup (dir, entry_name, &inode);
  dir_close (dir);
  return file_open (inode);
}

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
  bool success = dir_remove (dir, name);
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
