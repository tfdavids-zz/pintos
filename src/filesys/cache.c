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
struct cache_entry *cache_get (struct block *block, block_sector_t sector);
struct cache_entry *cache_remove (struct block *block, block_sector_t sector);
bool cache_contains (struct block *block, block_sector_t sector);
struct cache_entry *cache_evict (void);

void cache_init (void)
{
  rw_init (&cache_lock);
  list_init (&cache);
}

void cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  rw_reader_lock (&cache_lock);

  // check if cache contains block and sector
  struct cache_entry *c = cache_get (block, sector);
  if (c != NULL)
    {
      rw_reader_unlock (&cache_lock);
      memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
      c->accessed = true;
      rw_reader_unlock (&c->l);
      return;
    }

  rw_reader_unlock (&cache_lock);

  // didn't find it, so pop something
  rw_writer_lock (&cache_lock);
  c = cache_evict ();
  rw_writer_unlock (&cache_lock);
  struct cache_entry *c_copy = malloc (sizeof (struct cache_entry));
  
  c->block = block;
  c->sector = sector;
  c->loading = true;
  rw_writer_unlock (&c->l);

  rw_writer_lock (&cache_lock);
  ASSERT (!cache_contains (c->block, c->sector));
  list_push_back (&cache, &c->elem);
  rw_writer_unlock (&cache_lock);

  block_read (block, sector, c->data);
  memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
  rw_writer_lock (&c->l);
  c->loading = false;
  memcpy (c_copy, c, sizeof (struct cache_entry));
  rw_writer_unlock (&c->l);

  // thread_create ("read-ahead", PRI_MIN, cache_read_ahead, c_copy);
}

void cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  rw_writer_lock (&cache_lock);

  // check if cache contains block and sector
  struct cache_entry *c = cache_remove (block, sector);
  if (c != NULL)
    {
      rw_writer_unlock (&cache_lock);
      memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
      c->dirty = true;
      // block_write (block, sector, c->data);
      // c->dirty = false;
      c->writing_dirty = false;
      c->accessed = true;
      rw_writer_unlock (&c->l);
      rw_writer_lock (&cache_lock);
      ASSERT (!cache_contains (c->block, c->sector));
      list_push_back (&cache, &c->elem); // TODO: check size
      rw_writer_unlock (&cache_lock);
      return;
    }

  // didn't find it, so pop something
  c = cache_evict ();
  rw_writer_unlock (&cache_lock);
  
  c->block = block;
  c->sector = sector;
  c->loading = true;
  rw_writer_lock (&cache_lock);
  ASSERT (!cache_contains (c->block, c->sector));
  list_push_back (&cache, &c->elem);
  rw_writer_unlock (&cache_lock);
  memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
  c->dirty = true;
  // block_write (block, sector, c->data);
  // c->dirty = false;
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
struct cache_entry *cache_get (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  rw_reader_lock (&cache_lock);

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      rw_reader_lock (&c->l);
      if (c->block == block && c->sector == sector)
        {
          rw_reader_unlock (&cache_lock);
          return c;
        }
      else
        {
          rw_reader_unlock (&c->l);
        }
    }

  rw_reader_unlock (&cache_lock);

  return NULL;
}

// MUST HOLD CACHE WRITER LOCK (TODO: check)
struct cache_entry *cache_remove (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      rw_writer_lock (&c->l);
      if (c->block == block && c->sector == sector)
        {
          list_remove (&c->elem);
          return c;
        }
      else
        {
          rw_writer_unlock (&c->l);
        }
    }

  return NULL;
}

// MUST HOLD WRITER LOCK // TODO
struct cache_entry *cache_evict ()
{
  struct cache_entry *c;
  struct list_elem *e;

  if (list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      e = list_pop_front (&cache);
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
          e = list_pop_front (&cache);
          c = list_entry (e, struct cache_entry, elem);
        }
    }
  else
    {
      c = malloc (sizeof (struct cache_entry));
      rw_init (&c->l);
    }

  rw_writer_lock (&c->l);

  return c;
}

void cache_flush (void)
{
  rw_writer_lock (&cache_lock);

  struct cache_entry *c;
  struct list_elem *e;

  while (list_size (&cache) > 0)
    {
      e = list_pop_front (&cache);
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

  rw_writer_unlock (&cache_lock);
}

void cache_read_ahead (struct cache_entry *c)
{
  thread_current ()->background = true;
  struct block *block = c->block;
  block_sector_t sector = c->sector + 1;
  free (c);

  // check if cache contains block and sector
  rw_reader_lock (&cache_lock);
  if (cache_contains (block, sector))
    {
      rw_reader_unlock (&c->l);
      return;
    }

  rw_reader_unlock (&cache_lock);
  // TODO: race condition in here?
  rw_writer_lock (&cache_lock);

  // didn't find it, so pop something
  c = cache_evict ();
  
  c->block = block;
  c->sector = sector;
  c->loading = true;
  list_push_back (&cache, &c->elem);
  block_read (block, sector, c->data);
  c->loading = false;
  rw_writer_unlock (&c->l);
}

// MUST HOLD CACHE_LOCK!!! // TODO: grep for this
bool cache_contains (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          rw_reader_unlock (&cache_lock);
          return true;
        }
    }

  return false;
}

void cache_read_bytes (struct block *block, block_sector_t sector,
                       int sector_ofs, int chunk_size, void *buffer)
{
  struct cache_entry *c = cache_get (block, sector);  
  if (c != NULL)
    {
      memcpy (buffer, c->data + sector_ofs, chunk_size);
    }
  else
    {
  // didn't find it, so pop something
  rw_writer_lock (&cache_lock);
  c = cache_evict ();
  rw_writer_unlock (&cache_lock);
  struct cache_entry *c_copy = malloc (sizeof (struct cache_entry));
  
  c->block = block;
  c->sector = sector;
  c->loading = true;
  rw_writer_unlock (&c->l);

  rw_writer_lock (&cache_lock);
  ASSERT (!cache_contains (c->block, c->sector));
  list_push_back (&cache, &c->elem);
  rw_writer_unlock (&cache_lock);

  block_read (block, sector, c->data);
  memcpy (buffer, c->data + sector_ofs, chunk_size);
  rw_writer_lock (&c->l);
  c->loading = false;
  memcpy (c_copy, c, sizeof (struct cache_entry));
  rw_writer_unlock (&c->l);

  // thread_create ("read-ahead", PRI_MIN, cache_read_ahead, c_copy);
    }
} 
