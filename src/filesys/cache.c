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
static struct rw_lock cache_lock; /* Used to protect metadata */
static bool cache_full;

struct cache_entry
{
  struct list_elem elem;   /* For the cache list. */

  struct block *block;
  block_sector_t sector;
  bool accessed;                /* True if entry has been used recently. */
  bool loading;                 /* True if entry's data is being loaded. */
  bool dirty;                   /* True if entry's data has been modified. */
  char data[BLOCK_SECTOR_SIZE]; /* The cached data. */
  struct rw_lock l;             /* To synchronize access to the entry. */
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

/* Read SECTOR from BLOCK into BUFFER from the cache. If (BLOCK, SECTOR) is
   not in the cache, then load it inot the cache and then read it into BUFFER.
   */
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

/* Write BUFFER into the cache entry for (BLOCK, SECTOR). If no
   such entry exists, create one and put it into the cache. */
void cache_write (struct block *block, block_sector_t sector,
  const void *buffer)
{
  /* If the block is already cached, simply load the data into it. */
  struct cache_entry *c = cache_get_lock (block, sector, C_WRITE);
  if (c != NULL)
    {
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

/* Retrieve a cache entry for (BLOCK, SECTOR), with either its reader or writer
   lock held, as specified by LOCK_TYPE. Return NULL if not found. */
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

/* Insert an entry for (BLOCK, SECTOR) into the cache, if no such
   entry exists. Evict an entry if necessary, using the second-chance
   algorithm. Return with the cache entry's writer lock held; return
   NULL on failure. */
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

  /* Evict an entry if necessary. */
  if (cache_full || list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      cache_full = true;

      ASSERT (!list_empty (&cache));
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);

      while (c->loading || c->accessed || c->dirty)
        {
          /* Skip this entry if its data is being read in. */
          if (!c->loading)
            {
              c->accessed = false;
              if (c->dirty)
                {
                  block_write (c->block, c->sector, c->data);
                  c->dirty = false;
                  break;
                }
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
        {
          PANIC ("OOM: Failed to create cache_entry");
        }
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

/* Flush the cache back to disk. */
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
          block_write (c->block, c->sector, c->data);
          c->dirty = false;
          rw_writer_unlock (&c->l);
        }
      free (c);
    }

  cache_full = false;

  rw_writer_unlock (&cache_lock);
}

/* Read CHUNK_SIZE bytes from SECTOR_OFS in the entry for
   (BLOCK, SECTOR) into BUFFER. Create a cache entry if necessary. */
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

/* Write CHUNK_SIZE bytes from SECTOR_OFS for the cache entry
   (BLOCK, SECTOR) into BUFFER. Create a cache entry if necessary. */
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
