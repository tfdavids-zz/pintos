#include "filesys/cache.h"
#include <string.h>
#include <list.h>
#include <stdio.h>
#include "lib/stdbool.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/block.h"
#include "devices/timer.h"

#define NUM_CACHE_BLOCKS 64

#define C_READ 1
#define C_WRITE 2

static struct list cache;
static struct rw_lock cache_lock; // used for metadata
static bool cache_full;

struct cache_entry
{
  struct list_elem elem;   /* For the cache list. */
  struct list_elem d_elem; /* For the dirty list. */

  struct block *block;
  block_sector_t sector;
  bool accessed;                /* True if entry has been used recently. */
  bool loading;                 /* True if entry's data is being loaded. */
  bool dirty;                   /* True if entry's data has been modified. */
  bool writing_dirty;           /* True if writing to disk. */
  bool should_read_ahead;
  char data[BLOCK_SECTOR_SIZE]; /* The cached data. */
  struct rw_lock l;                  /* To synchronize access to the entry. */
};

void cache_write_dirty (void *aux);
struct cache_entry *cache_get_lock (struct block *block, block_sector_t sector,
  int lock_type);
struct cache_entry *cache_insert_write_lock (struct block *block,
  block_sector_t sector);

void cache_init (void)
{
  rw_init (&cache_lock);
  list_init (&cache);
}

void cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  /* If the block is already cached, simply read its entry. */
  struct cache_entry *c = cache_get_lock (block, sector, C_READ);
  if (c != NULL)
    {
      memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
      c->accessed = true;
      rw_reader_unlock (&c->l);
      return;
    }

  /* Otherwise, create a cache entry for this block. */
  c = cache_insert_write_lock (block, sector);
  block_read (block, sector, c->data);
  memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
  c->loading = false;
  rw_writer_unlock (&c->l);
}

void cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  // check if cache contains block and sector
  struct cache_entry *c = cache_get_lock (block, sector, C_WRITE);
  if (c != NULL)
    {
      c->writing_dirty = false;
      c->accessed = true;
      memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
      c->dirty = true;
      rw_writer_unlock (&c->l);
      return;
    }

  /* Otherwise, load it into the cache. */
  c = cache_insert_write_lock (block, sector);
  memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
  c->loading = false;
  c->dirty = true;
  rw_writer_unlock (&c->l);
}

// Retrieves a cache entry, with either its reader or writer lock held, as
// specified by LOCK_TYPE.
struct cache_entry *cache_get_lock (struct block *block, block_sector_t sector,
  int lock_type)
{
  struct list_elem *e;
  struct cache_entry *c;

  rw_reader_lock (&cache_lock);
  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          if (lock_type == C_READ)
            {
              rw_reader_lock (&c->l);
            }
          else
            {
              rw_writer_lock (&c->l);
            }
          rw_reader_unlock (&cache_lock);
          return c;
        }
    }
  rw_reader_unlock (&cache_lock);

  return NULL;
}

struct cache_entry *cache_insert_write_lock (struct block *block,
  block_sector_t sector)
{
  struct cache_entry *c;
  struct list_elem *e;

  rw_writer_lock (&cache_lock);

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          rw_writer_unlock (&cache_lock);
          return NULL;
        }
    }

  if (cache_full || list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      cache_full = true;

      ASSERT (!list_empty (&cache));
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);

      while (c->loading || c->accessed || c->dirty)
        {
          if (c->writing_dirty || c->loading || c->should_read_ahead)
            {
              // let I/O finish
            }
          else
            {
              if (c->dirty)
                {
                  block_write (c->block, c->sector, c->data);
                  c->dirty = false;
                  break;
                }
              c->accessed = false;
            }

          list_push_back (&cache, e);
          ASSERT (!list_empty (&cache));
          e = list_pop_front (&cache);
          c = list_entry (e, struct cache_entry, elem);
        }
    }
  else
    {
      c = malloc (sizeof (struct cache_entry));
      if (c == NULL)
        PANIC ("failed to create cache_entry");
      rw_init (&c->l);
    }

  rw_writer_lock (&c->l);
  list_push_back (&cache, &c->elem);
  c->sector = sector;
  c->block = block;
  c->loading = true;
  rw_writer_unlock (&cache_lock);

  return c;
}

void cache_flush (void)
{
  rw_writer_lock (&cache_lock);

  struct cache_entry *c;
  struct list_elem *e;

  /* Clear the cache. */
  while (list_size (&cache) > 0)
    {
      ASSERT (!list_empty (&cache));
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);

      if (c->dirty)
        {
          rw_writer_lock (&c->l);
          c->writing_dirty = true;
          block_write (c->block, c->sector, c->data);
          c->dirty = false;
          c->writing_dirty = false;
          rw_writer_unlock (&c->l);
        }

      free (c);
    }

  cache_full = false;

  rw_writer_unlock (&cache_lock);
}

void cache_read_bytes (struct block *block, block_sector_t sector,
                       int sector_ofs, int chunk_size, void *buffer)
{
  struct cache_entry *c = cache_get_lock (block, sector, C_READ);
  if (c != NULL)
    {
      memcpy (buffer, c->data + sector_ofs, chunk_size);
      rw_reader_unlock (&c->l);
    }
  else
    {
      c = cache_insert_write_lock (block, sector);

      block_read (block, sector, c->data);
      memcpy (buffer, c->data + sector_ofs, chunk_size);
      c->loading = false;
      rw_writer_unlock (&c->l);
    }
}

void cache_write_bytes (struct block *block, block_sector_t sector,
                        int sector_ofs, int chunk_size, const void *buffer)
{
  struct cache_entry *c = cache_get_lock (block, sector, C_WRITE);
  if (c != NULL)
    {
      memcpy (c->data + sector_ofs, buffer, chunk_size);
      c->dirty = true;
      rw_writer_unlock (&c->l);
    }
  else
    {
      c = cache_insert_write_lock (block, sector);

      block_read (block, sector, c->data);
      memcpy (c->data + sector_ofs, buffer, chunk_size);
      c->loading = false;
      c->dirty = true;
      rw_writer_unlock (&c->l);
    }
}
