#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define PTRS_PER_INODE 14
#define DIRECT_BLOCKS 12
#define INDIR_BLOCK_INDEX 12
#define DOUBLY_INDIR_BLOCK_INDEX 13
#define PTRS_PER_INDIR_BLOCK 128


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t block_ptrs[PTRS_PER_INODE];     /* Block ptrs in inode */
    off_t length;                                  /* File size in bytes. */
    unsigned magic;                                /* Magic number. */
    uint32_t sectors;                              /* Number of data sectors */
    uint32_t type;                                 /* I_FILE or I_DIR */
    uint32_t unused[110];                          /* Not used. */
  };


/* A block of zeros, used for initalizing memory */
static char zeros[BLOCK_SECTOR_SIZE];

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* Returns the index of the block pos is in
 */
static inline size_t
pos_to_block (off_t pos)
{
  return pos / BLOCK_SECTOR_SIZE;
}


/* Returns the index of the block within the indir block
   (meaningful only if pos is in an indirect block)
 */
static inline size_t
get_indir_off (size_t block)
{
  return (block - DIRECT_BLOCKS) % PTRS_PER_INDIR_BLOCK;
}


/* Returns the index of the indir block within the doubly indir
   block
   (meaningful only if pos is in an indirect block)
 */
static inline size_t
get_doubly_indir_off (size_t block)
{
  return (block - DIRECT_BLOCKS - PTRS_PER_INDIR_BLOCK) / PTRS_PER_INDIR_BLOCK;
}


/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock lock;                   /* Synch access to inode */
  };

/* Indirect and doubly indirect blocks on disk */
struct indir_block_disk
  {
    block_sector_t block_ptrs[PTRS_PER_INDIR_BLOCK];    /* Block ptrs in indir block */
  };


size_t inode_grow (struct inode_disk *, off_t);
bool inode_grow_dir_blocks (struct inode_disk *, size_t);
bool inode_grow_indir_blocks (struct inode_disk *, size_t);
bool inode_grow_doubly_indir_blocks (struct inode_disk *, size_t);
size_t inode_grow_indir_block (struct indir_block_disk *, size_t, size_t);
bool inode_free (struct inode_disk *);
bool inode_free_dir_blocks (struct inode_disk *disk_inode);
bool inode_free_indir_blocks (struct inode_disk *disk_inode);
bool inode_free_doubly_indir_blocks (struct inode_disk *disk_inode);
size_t inode_free_indir_block (struct indir_block_disk *, size_t);
void* calloc_wrapper (size_t, size_t);


/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS.

   Precondition: Caller holds inode lock. */
static block_sector_t
byte_to_sector (struct inode_disk *disk_inode, off_t pos)
{
  struct indir_block_disk *indir_block;
  struct indir_block_disk *doubly_indir_block;

  indir_block = calloc_wrapper (1, sizeof *indir_block);
  doubly_indir_block = calloc_wrapper (1, sizeof *doubly_indir_block);

  size_t block = pos_to_block (pos);
  size_t indir_off = get_indir_off (block);
  size_t doubly_indir_off = get_doubly_indir_off (block);

  block_sector_t sector_t = -1;

  if (pos < disk_inode->length)
    {
      if (block < DIRECT_BLOCKS)
        {
          sector_t = disk_inode->block_ptrs[block];
        }
      else if (block < DIRECT_BLOCKS + PTRS_PER_INDIR_BLOCK)
        {
          /* Read indirect block */
          block_read (fs_device, disk_inode->block_ptrs[INDIR_BLOCK_INDEX],
                      indir_block);
          sector_t = indir_block->block_ptrs[indir_off];          
        }
      else if (block < DIRECT_BLOCKS + PTRS_PER_INDIR_BLOCK +
               PTRS_PER_INDIR_BLOCK * PTRS_PER_INDIR_BLOCK)
        {
          /* Read doubly indirect block */
          block_read (fs_device, disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX],
                      doubly_indir_block);      
          /* Read indirect block */
          block_read (fs_device,
                      doubly_indir_block->block_ptrs[doubly_indir_off],
                      indir_block);
          sector_t = indir_block->block_ptrs[indir_off];
        }
    }
  free (indir_block);
  free (doubly_indir_block);

  return sector_t;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct rw_lock rw_l;


/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  rw_init (&rw_l);
  // cache_init ();
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t type)
{
  struct inode_disk *disk_inode;
  bool success = false;
  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc_wrapper (1, sizeof *disk_inode);

  disk_inode->length = 0;
  disk_inode->sectors = 0;
  disk_inode->magic = INODE_MAGIC;
  disk_inode->type = type;
  block_write (fs_device, sector, disk_inode);

  /* Expand inode to given length */
  if (inode_grow (disk_inode, length) != (size_t) length)
    {
      /* Delete the inode */
      success = false;
      block_write (fs_device, sector, zeros);
    }
  else
    {
      success = true;
      disk_inode->sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      block_write (fs_device, sector, disk_inode);
    }

  free (disk_inode);

  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;
  /* Check whether this inode is already open. */
  rw_reader_lock (&rw_l);
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          rw_reader_unlock (&rw_l);
          return inode; 
        }
    }
    rw_reader_unlock (&rw_l);

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  rw_writer_lock (&rw_l);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_init(&inode->lock);
  list_push_front (&open_inodes, &inode->elem);
  rw_writer_unlock (&rw_l);

  return inode;
}


/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&inode->lock);
      inode->open_cnt++;
      lock_release (&inode->lock);
    }
  return inode;
}

/* Returns the open count for INODE. */
size_t
inode_open_count (struct inode *inode)
{
  lock_acquire (&inode->lock);
  size_t count = inode->open_cnt;
  lock_release (&inode->lock);
  return count;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (struct inode *inode)
{
  lock_acquire (&inode->lock);
  block_sector_t sector = inode->sector;
  lock_release (&inode->lock);
  return sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;
  
  /* Release resources if this was the last opener. */
  rw_writer_lock (&rw_l);
  lock_acquire (&inode->lock);
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      rw_writer_unlock (&rw_l);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          struct inode_disk *disk_inode =
            calloc_wrapper (1, sizeof *disk_inode);
          block_read (fs_device, inode->sector, disk_inode);
          inode_free (disk_inode);
          block_write (fs_device, inode->sector, zeros);
          free_map_release (inode->sector, 1);
          free (disk_inode);
        }
      lock_release (&inode->lock);
      free (inode); 
    }
  else
    {
      rw_writer_unlock (&rw_l);
      lock_release (&inode->lock);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  ASSERT (inode != NULL);
  inode->removed = true;
  lock_release (&inode->lock);
}

/* Returns true if and only if the inode has been removed. */
bool
inode_is_removed (struct inode *inode)
{
  ASSERT (inode != NULL);
  lock_acquire (&inode->lock);
  bool ret = inode->removed;
  lock_release (&inode->lock);
  return ret;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  lock_acquire (&inode->lock);

  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  struct inode_disk *disk_inode;
  disk_inode = calloc_wrapper (1, sizeof *disk_inode);
  block_read (fs_device, inode->sector, disk_inode); /* TODO: cache_read */

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;
      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);
  free (disk_inode);
  lock_release (&inode->lock);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Grows the file to the appropriate size(not above max size)
   if necessary
   Returns the number of bytes actually written
*/
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;
  bool grown = false;
  struct inode_disk *disk_inode;

  if (inode->deny_write_cnt)
    {
      return 0;
    }

  disk_inode = calloc_wrapper (1, sizeof *disk_inode);
  block_read (fs_device, inode->sector, disk_inode);

  /* Grow the file if beyond EOF */
  if (offset + size > disk_inode->length)
    {
      /* Only lock if we are trying to grow the file */
      lock_acquire (&inode->lock);
      disk_inode->length = inode_grow (disk_inode, offset + size);
      grown = true;
    }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (disk_inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = disk_inode->length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);
  if (grown)
    {
      block_write (fs_device, inode->sector, disk_inode);
      lock_release (&inode->lock);
    }

  free (disk_inode);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release (&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  lock_acquire (&inode->lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release (&inode->lock);
}

/* Returns the length, in bytes, of INODE's data. */
/* Precondition: Caller must hold inode lock. */
off_t
inode_length (struct inode *inode)
{
  size_t length;
  struct inode_disk *disk_inode;
  disk_inode = calloc_wrapper (1, sizeof *disk_inode);

  block_read (fs_device, inode->sector, disk_inode);

  length = disk_inode->length;
  free (disk_inode);
  return length;
}


/* Given an inode and length
   Grows the inode(with zero filled blocks) to that length
   Returns new length
*/

size_t
inode_grow (struct inode_disk *disk_inode, off_t new_length)
{
  size_t new_length_sectors = bytes_to_sectors (new_length);
  bool success = true;

  if (disk_inode->sectors < new_length_sectors)
    {
      success = inode_grow_dir_blocks (disk_inode, new_length_sectors);
    }

  if (disk_inode->sectors < new_length_sectors && success)
    {
      success = inode_grow_indir_blocks (disk_inode, new_length_sectors);
    }

  if (disk_inode->sectors < new_length_sectors && success)
    {
      success = inode_grow_doubly_indir_blocks (disk_inode, new_length_sectors);
    }

  if (disk_inode->sectors == new_length_sectors)
    {
      /* Return new_length if successfully added new_length_sectors blocks*/
      return new_length;
    }
  else
    {
      /* Return the number of allocted bytes in the inode */
      return disk_inode->sectors * BLOCK_SECTOR_SIZE;
    }
}

/* Given an array of direct block_ptrs, grows inode by allocating new
   pages
   return: true on success
*/
bool
inode_grow_dir_blocks (struct inode_disk *disk_inode, size_t new_length_sectors)
{
  size_t i;
  size_t next_block = disk_inode->sectors;

  /* Fill necessary dir blocks with zero */
  for (i = next_block; i < DIRECT_BLOCKS &&
      disk_inode->sectors < new_length_sectors; i++)
    {
      if (free_map_allocate (1, &disk_inode->block_ptrs[i]))
        {
          block_write (fs_device, disk_inode->block_ptrs[i], zeros);
          disk_inode->sectors++;
        }
      else
        {
          return false;
        }
    }

  return true;
}

/* Given inode attempt to grow the indirect block to accommodate given number
   of new sectors. Called when direct blocks are full.
   return: true on success */
bool
inode_grow_indir_blocks (struct inode_disk *disk_inode,
  size_t new_length_sectors)
{
  size_t next_block = disk_inode->sectors;
  size_t indir_off = get_indir_off (next_block);
  size_t new_sectors = new_length_sectors - disk_inode->sectors;
  size_t added_sectors = 0;
  bool success = false;
  struct indir_block_disk *indir_block;

  /* No room in the indirect blook */
  if (next_block >= DIRECT_BLOCKS + PTRS_PER_INDIR_BLOCK)
    {
      return true;
    }
  indir_block = calloc_wrapper (1, sizeof *indir_block);

  /* Create an indir block if necessary */
  if (next_block == INDIR_BLOCK_INDEX)
    {
      if (!free_map_allocate (1, &disk_inode->block_ptrs[INDIR_BLOCK_INDEX]))
        {
          goto done;
        }
      block_write (fs_device, disk_inode->block_ptrs[INDIR_BLOCK_INDEX],
                   zeros);
    }

  /* Read indirect block */
  block_read (fs_device, disk_inode->block_ptrs[INDIR_BLOCK_INDEX],
              indir_block);

  added_sectors = inode_grow_indir_block (indir_block,
                                          indir_off,
                                          new_sectors);

  block_write (fs_device, disk_inode->block_ptrs[INDIR_BLOCK_INDEX],
               indir_block);

  disk_inode->sectors += added_sectors;
  success = true;
 done:
  free (indir_block);
  return success;
}

/* Given inode attempts to grow the doubly indirect block to accommodate given
   number of sectors. Called when direct blocks and the indrect block are full
   return: true on success */
bool
inode_grow_doubly_indir_blocks (struct inode_disk *disk_inode,
  size_t new_length_sectors)
{
  size_t i;
  size_t next_block = disk_inode->sectors;
  size_t indir_off = get_indir_off (next_block);
  size_t doubly_indir_off = get_doubly_indir_off (next_block);
  size_t new_sectors = new_length_sectors - disk_inode->sectors;
  size_t added_sectors = 0;
  bool success = false;

  /* No room in the doubly indirect block */
  if (next_block >= DIRECT_BLOCKS + PTRS_PER_INDIR_BLOCK +
      PTRS_PER_INDIR_BLOCK * PTRS_PER_INDIR_BLOCK)
    {
      return true;
    }
  struct indir_block_disk *indir_block;
  struct indir_block_disk *doubly_indir_block;

  indir_block = calloc_wrapper (1, sizeof *indir_block);
  doubly_indir_block = calloc_wrapper (1, sizeof *doubly_indir_block);

  /* Create a doubly indir block if necessary */
  if (doubly_indir_off == 0 && indir_off == 0)
    {
      if (!free_map_allocate (1,
        &disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX]))
        {
          goto done;
        }
      block_write (fs_device, disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX],
                   zeros);
    }

  /* Read doubly indirect block */
  block_read (fs_device, disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX],
              doubly_indir_block);
  for (i = doubly_indir_off;
       i < PTRS_PER_INDIR_BLOCK && disk_inode->sectors < new_length_sectors;
       i++)
    {
      if (indir_off == 0)
        {
          if (!free_map_allocate (1, &doubly_indir_block->block_ptrs[i]))
            {
              goto done;
            }
          block_write (fs_device, doubly_indir_block->block_ptrs[i], zeros);
        }
      block_read (fs_device, doubly_indir_block->block_ptrs[i], indir_block);
      disk_inode->sectors += inode_grow_indir_block (indir_block,
                               indir_off,
                               (new_sectors - added_sectors));

      block_write (fs_device, doubly_indir_block->block_ptrs[i], indir_block);
      indir_off = get_indir_off (disk_inode->sectors);
    }

  block_write (fs_device, disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX],
               doubly_indir_block);
  success = true;
 done:
  free (doubly_indir_block);
  free (indir_block);

  return success;
}


/* Helper function that grows indir_block_disk by allocating
   new bocks
   Returns: number of successfully added blocks
*/
size_t
inode_grow_indir_block (struct indir_block_disk *indir_block,
                        size_t indir_off,
                        size_t new_sectors)
{
  size_t i;
  size_t added_sectors = 0;
  for (i = indir_off;
       i < PTRS_PER_INDIR_BLOCK && added_sectors < new_sectors;
       i++)
    {
      if (free_map_allocate (1, &indir_block->block_ptrs[i]))
        {
          block_write (fs_device, indir_block->block_ptrs[i], zeros);
          added_sectors++;
        }
      else
        {
          break;
        }
    }
  return added_sectors;
}
/* Given inode frees(overwrites with zeros) all of its blocks */
bool
inode_free (struct inode_disk *disk_inode)
{
  bool success = true;

  if (disk_inode->sectors > DIRECT_BLOCKS + PTRS_PER_INDIR_BLOCK)
    {
      success = success && inode_free_doubly_indir_blocks (disk_inode);
    }
  if (disk_inode->sectors > DIRECT_BLOCKS)
    {
      success = success && inode_free_indir_blocks (disk_inode);
    }
  if (disk_inode->sectors > 0)
    {
      success = success && inode_free_dir_blocks (disk_inode);
    }

  return success;
}

/* Frees all blocks dir blocks of the given inode
return: true on success
*/
bool
inode_free_dir_blocks (struct inode_disk *disk_inode)
{
  size_t sectors = disk_inode->sectors;
  size_t i;

  /* Fill necessary dir blocks with zero */
  for (i = 0; i < sectors && i < DIRECT_BLOCKS; i++)
    {
      block_write (fs_device, disk_inode->block_ptrs[i], zeros); 
      free_map_release (disk_inode->block_ptrs[i], 1);
      disk_inode->sectors--;
    }

  return true;
}

/* Frees all blocks pointed by the indir block of the given inode 
return: true on success
*/
bool
inode_free_indir_blocks (struct inode_disk *disk_inode)
{
  size_t last_block = disk_inode->sectors - 1;
  size_t indir_off = get_indir_off (last_block);
  size_t freed_sectors = 0;
  bool success = false;

  struct indir_block_disk *indir_block;
  indir_block = calloc_wrapper (1, sizeof *indir_block);

  /* Read indirect block */
  block_read (fs_device, disk_inode->block_ptrs[INDIR_BLOCK_INDEX],
              indir_block);
  
  freed_sectors = inode_free_indir_block (indir_block, indir_off);

  disk_inode->sectors -= freed_sectors;
  block_write (fs_device, disk_inode->block_ptrs[INDIR_BLOCK_INDEX],
               zeros);
  free_map_release (disk_inode->block_ptrs[INDIR_BLOCK_INDEX], 1);

  success = true;

  free (indir_block);
  return success;
}

/* Frees all blocks pointed by the doubly indir block of the given inode 
return: true on success
*/
bool
inode_free_doubly_indir_blocks (struct inode_disk *disk_inode)
{
  size_t i;
  size_t last_block = disk_inode->sectors;

  size_t indir_off = get_indir_off (last_block);
  size_t doubly_indir_off = get_doubly_indir_off (last_block);

  size_t freed_sectors = 0;

  struct indir_block_disk *indir_block;
  struct indir_block_disk *doubly_indir_block;
  
  indir_block = calloc_wrapper (1, sizeof *indir_block);
  doubly_indir_block = calloc_wrapper (1, sizeof *doubly_indir_block);

  block_read (fs_device, disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX],
              doubly_indir_block);

  for (i = 0; i < doubly_indir_off; i++)
    {
      block_read (fs_device, doubly_indir_block->block_ptrs[i], indir_block);
      freed_sectors = inode_free_indir_block (indir_block, indir_off);
      block_write (fs_device, doubly_indir_block->block_ptrs[i], zeros);
      free_map_release (doubly_indir_block->block_ptrs[i], 1);

      disk_inode->sectors -= freed_sectors;
      indir_off = get_indir_off (disk_inode->sectors);
    }

  block_write (fs_device, disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX],
               zeros);
  free_map_release (disk_inode->block_ptrs[DOUBLY_INDIR_BLOCK_INDEX], 1);

  return true;
}

/* Helper function that free indir_block_disk
   Returns: num of freed blocks
*/
size_t
inode_free_indir_block (struct indir_block_disk *indir_block,
                        size_t indir_off)
{  
  size_t i;
  size_t freed_sectors = 0;
  for (i = 0; i < indir_off; i++)
    {
      block_write (fs_device, indir_block->block_ptrs[i], zeros); 
      free_map_release (indir_block->block_ptrs[i], 1);
      freed_sectors++;
    }

  return freed_sectors;
}


void*
calloc_wrapper (size_t cnt, size_t size)
{
  void *ptr = calloc (cnt, size);
  if (ptr == NULL)
    {
      PANIC ("Ran out of kernel memory.");
    }
  return ptr;
}

/* Returns true if INODE backs a file, false if it backs
   a directory. */
bool
inode_is_file (struct inode *inode)
{
  struct inode_disk disk_inode;
  lock_acquire (&inode->lock);
  block_read (fs_device, inode->sector, &disk_inode);
  bool ret = (disk_inode.type == I_FILE);
  lock_release (&inode->lock);

  return ret;
}
