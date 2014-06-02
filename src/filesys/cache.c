#include "filesys/cache.h"
#include <string.h>
#include <list.h>
#include <stdio.h>
#include "lib/stdbool.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "devices/block.h"

#define NUM_CACHE_BLOCKS 64

/* reader-writer locks */

struct rw
{
  int num_readers, num_writers, num_waiting_writers;
  struct lock l;
  struct condition can_read, can_write;
};

void rw_init (struct rw *lock)
{
  lock_init (&lock->l);
  cond_init (&lock->can_read);
  cond_init (&lock->can_write);
  lock->num_readers = 0;
  lock->num_writers = 0;
  lock->num_waiting_writers = 0;
}

void rw_writer_lock (struct rw *lock)
{
  lock_acquire (&lock->l);
  lock->num_waiting_writers++;
  while (lock->num_readers > 0 || lock->num_writers > 0)
    cond_wait (&lock->can_write, &lock->l);
  lock->num_waiting_writers--;
  lock->num_writers++;
  lock_release (&lock->l);
}

void rw_writer_unlock (struct rw *lock)
{
  lock_acquire (&lock->l);
  lock->num_writers--;
  if (lock->num_waiting_writers > 0)
    cond_signal (&lock->can_write, &lock->l);
  else
    cond_broadcast (&lock->can_read, &lock->l);
  lock_release (&lock->l);
}

void rw_reader_lock (struct rw *lock)
{
  lock_acquire (&lock->l);
  while (lock->num_writers > 0 || lock->num_waiting_writers > 0)
    cond_wait (&lock->can_read, &lock->l);
  lock->num_readers++;
  lock_release (&lock->l);
}

void rw_reader_unlock (struct rw *lock)
{
  lock_acquire (&lock->l);
  lock->num_readers--;
  if (lock->num_readers == 0)
    cond_signal (&lock->can_write, &lock->l);
  lock_release (&lock->l);
}

/* end read-writer locks */

static struct list cache;
static struct rw cache_lock; // used for metadata
static bool cache_full;

struct cache_entry
{
  struct list_elem elem;

  struct block *block;
  block_sector_t sector;
  bool accessed;
  bool loading;
  bool dirty;
  bool writing_dirty; // if we're in the process of writing to disk
  char data[BLOCK_SECTOR_SIZE];
  struct rw l; // used for working with data[]
};

void cache_write_dirty (struct cache_entry *c);
void cache_read_ahead (struct cache_entry *c);
struct cache_entry *cache_get_read (struct block *block, block_sector_t sector);
struct cache_entry *cache_get_write (struct block *block, block_sector_t sector);
struct cache_entry *cache_insert (struct block *block, block_sector_t sector);

void cache_init (void)
{
  rw_init (&cache_lock);
  list_init (&cache);
}

void cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  rw_reader_lock (&cache_lock);
  
  // check if cache contains block and sector
  struct cache_entry *c = cache_get_read (block, sector);
  if (c != NULL)
    {
      rw_reader_unlock (&cache_lock);
      memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
      c->accessed = true;
      rw_reader_unlock (&c->l);
      return;
    }

  rw_reader_unlock (&cache_lock);

  c = cache_insert (block, sector);
  block_read (block, sector, c->data);
  memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
  c->loading = false;
  struct cache_entry *c_copy = malloc (sizeof (struct cache_entry));
  memcpy (c_copy, c, sizeof (struct cache_entry));
  rw_writer_unlock (&c->l);

  thread_create ("read-ahead", PRI_MIN, cache_read_ahead, c_copy);
}

void cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  rw_reader_lock (&cache_lock);

  // check if cache contains block and sector
  struct cache_entry *c = cache_get_write (block, sector);
  if (c != NULL)
    {
      c->dirty = true;
      c->writing_dirty = false;
      c->accessed = true;
      rw_reader_unlock (&cache_lock);
      memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
      rw_writer_unlock (&c->l);
      return;
    }
  rw_reader_unlock (&cache_lock);

  c = cache_insert (block, sector);
  memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
  c->dirty = true;
  c->loading = false;
  rw_writer_unlock (&c->l);
}

void cache_write_dirty (struct cache_entry *c)
{
  thread_current ()->background = true;
  rw_reader_lock (&c->l);
  ASSERT (c->dirty);
  block_write (c->block, c->sector, c->data);
  c->dirty = false;
  c->writing_dirty = false;
  rw_reader_unlock (&c->l);
}

// RETURNS WITH READER LOCK
struct cache_entry *cache_get_read (struct block *block, block_sector_t sector)
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
          rw_reader_lock (&c->l);
          rw_reader_unlock (&cache_lock);
          return c;
        }
    }

  rw_reader_unlock (&cache_lock);

  return NULL;
}

// RETURNS WITH WRITER LOCK
struct cache_entry *cache_get_write (struct block *block, block_sector_t sector)
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
          rw_writer_lock (&c->l);
          rw_reader_unlock (&cache_lock);
          return c;
        }
    }

  rw_reader_unlock (&cache_lock);

  return NULL;
}

// MUST HOLD WRITER LOCK // TODO
struct cache_entry *cache_insert (struct block *block, block_sector_t sector)
{
  struct cache_entry *c;
  struct list_elem *e;

  rw_writer_lock (&cache_lock);

  if (cache_full || list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      cache_full = true;

      // printf ("checkpoint 1: open . . .");
      e = list_pop_front (&cache);
      // printf (" and close!\n");
      c = list_entry (e, struct cache_entry, elem);

      while (c->loading || c->accessed || c->dirty)
        {
          if (c->writing_dirty || c->loading)
            {
              // do nothing (wait for IO)
            }
          else if (c->dirty)
            {
              c->writing_dirty = true;
              thread_create ("write-behind", PRI_DEFAULT, cache_write_dirty, c);
            }
          else if (c->accessed)
            {
              c->accessed = false;
            }

          list_push_back (&cache, e);
          // printf ("checkpoint 2: open . . .");
          e = list_pop_front (&cache);
          // printf (" and close!\n");
          c = list_entry (e, struct cache_entry, elem);
        }
    }
  else
    {
      c = malloc (sizeof (struct cache_entry));
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

  while (list_size (&cache) > 0)
    {
      // printf ("checkpoint 3: open . . .");
      e = list_pop_front (&cache);
      // printf (" and close!\n");
      c = list_entry (e, struct cache_entry, elem);
      if (c->dirty)
        {
          rw_reader_lock (&c->l);
          c->writing_dirty = true;
          block_write (c->block, c->sector, c->data);
          c->dirty = false;
          c->writing_dirty = false;
          rw_reader_unlock (&c->l);
        }

      free (c);
    }

  cache_full = false;

  rw_writer_unlock (&cache_lock);
}

void cache_read_ahead (struct cache_entry *c)
{
  thread_current ()->background = true;
  struct block *block = c->block;
  block_sector_t sector = c->sector + 1;
  free (c);

  // check if cache contains block and sector
  c = cache_get_read (block, sector);
  if (c != NULL)
    {
      rw_reader_unlock (&c->l);
      return;
    }

  c = cache_insert (block, sector);
  
  block_read (block, sector, c->data);
  c->loading = false;
  rw_writer_unlock (&c->l);
}

void cache_read_bytes (struct block *block, block_sector_t sector,
                       int sector_ofs, int chunk_size, void *buffer)
{
  struct cache_entry *c = cache_get_read (block, sector);  
  if (c != NULL)
    {
      memcpy (buffer, c->data + sector_ofs, chunk_size);
      rw_reader_unlock (&c->l);
    }
  else
    {
      c = cache_insert (block, sector);
      struct cache_entry *c_copy = malloc (sizeof (struct cache_entry));
    
      block_read (block, sector, c->data);
      memcpy (buffer, c->data + sector_ofs, chunk_size);
      c->loading = false;
      memcpy (c_copy, c, sizeof (struct cache_entry));
      rw_writer_unlock (&c->l);
    
      thread_create ("read-ahead", PRI_MIN, cache_read_ahead, c_copy);
    }
} 
