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
  //printf ("waiting to write %#x\n", lock);
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
  //printf ("write-unlocking %#x\n", lock);
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
  //printf ("waiting to read %#x\n", lock);
  lock_acquire (&lock->l);
  while (lock->num_writers > 0 || lock->num_waiting_writers > 0)
    cond_wait (&lock->can_read, &lock->l);
  lock->num_readers++;
  lock_release (&lock->l);
}

void rw_reader_unlock (struct rw *lock)
{
  //printf ("read-unlocking %#x\n", lock);
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

static bool running; /* For communicating with background threads. */

/* For writing dirty cache entries encountered during eviction. */
static struct list dirty_queue;
static struct condition dirty_queue_empty;
static struct lock dirty_queue_lock;

/* For reading ahead. */
static struct list read_queue;
static struct condition read_queue_empty;
static struct lock read_queue_lock;

struct cache_entry
{
  struct list_elem elem;   /* For the cache list. */
  struct list_elem d_elem; /* For the dirty list. */
  struct list_elem r_elem; /* For the read-ahead list. */

  struct block *block;
  block_sector_t sector;
  bool accessed;                /* True if entry has been used recently. */
  bool loading;                 /* True if entry's data is being loaded. */
  bool dirty;                   /* True if entry's data has been modified. */
  bool writing_dirty;           /* True if writing to disk. */
  char data[BLOCK_SECTOR_SIZE]; /* The cached data. */
  struct rw l;                  /* To synchronize access to the entry. */
};

void cache_write_dirty (void *aux);
struct cache_entry *cache_get_lock (struct block *block, block_sector_t sector,
  int lock_type);
struct cache_entry *cache_insert_write_lock (struct block *block,
  block_sector_t sector);
void cache_write_periodically (void *aux);
void cache_read_ahead (void *aux);

void cache_init (void)
{
  rw_init (&cache_lock);
  list_init (&cache);
  list_init (&dirty_queue);
  list_init (&read_queue);
  cond_init (&dirty_queue_empty);
  cond_init (&read_queue_empty);
  lock_init (&dirty_queue_lock);
  lock_init (&read_queue_lock);
  running = true;

  thread_create ("write-behind", PRI_DEFAULT, cache_write_dirty, NULL); /* TODO */
  thread_create ("read-ahead", PRI_MIN, cache_read_ahead, NULL);
  // thread_create ("write-periodically", PRI_MIN, cache_write_periodically, NULL);
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
  block_read (block, sector, c->data); /* TODO: Synchronize this gracefully. */
  memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
  c->loading = false;
  rw_writer_unlock (&c->l);


  // read-ahead
  c = cache_insert_write_lock (block, sector+1);
  if (c == NULL)
    return;
  rw_writer_unlock (&c->l);

  //lock_acquire (&read_queue_lock);
  //list_push_back (&read_queue, &c->r_elem);
  //cond_signal (&read_queue_empty, &read_queue_lock);
  //lock_release (&read_queue_lock);
}

void cache_write (struct block *block, block_sector_t sector, const void *buffer)
{

  // check if cache contains block and sector
  struct cache_entry *c = cache_get_lock (block, sector, C_WRITE);
  if (c != NULL)
    {
      c->dirty = true;
      c->writing_dirty = false;
      c->accessed = true;
      memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
      rw_writer_unlock (&c->l);
      return;
    }

  /* Otherwise, load it into the cache. */
  c = cache_insert_write_lock (block, sector);
  memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
  c->dirty = true;
  c->loading = false;
  rw_writer_unlock (&c->l);
}

/* TODO */
void
cache_write_dirty (void *aux)
{
 /* TODO: I suspect that a background thread will not free all
    the resources that it is supposed to free. */
  thread_current ()->background = true;

  while (running)
  {
    lock_acquire (&dirty_queue_lock);
    while (running && list_empty (&dirty_queue))
      {
        cond_wait (&dirty_queue_empty, &dirty_queue_lock);
      }

    if (list_empty (&dirty_queue))
      {
        // running must be false, so quit
        lock_release (&dirty_queue_lock);
        break;
      }

    struct list_elem *e;
    struct cache_entry *c;

    for (e = list_pop_front (&dirty_queue); !list_empty (&dirty_queue);
      e = list_pop_front (&dirty_queue))
      {
        c = list_entry (e, struct cache_entry, d_elem);
        rw_reader_lock (&c->l);
        ASSERT (c->dirty);
        block_write (c->block, c->sector, c->data);
        c->dirty = false; /* TODO: Should hold writer lock? */
        c->writing_dirty = false;
        rw_reader_unlock (&c->l);
      }
    lock_release (&dirty_queue_lock);
  }
}

/* TODO */
void
cache_read_ahead (void *aux)
{
 /* TODO: I suspect that a background thread will not free all
    the resources that it is supposed to free. */
  thread_current ()->background = true;

  while (running)
  {
    lock_acquire (&read_queue_lock);
    while (running && list_empty (&read_queue))
      {
        cond_wait (&read_queue_empty, &read_queue_lock);
      }

    struct list_elem *e;
    struct cache_entry *c;
    for (e = list_pop_front (&read_queue); !list_empty (&read_queue);
      e = list_pop_front (&read_queue))
      {
        c = list_entry (e, struct cache_entry, r_elem);
        rw_writer_lock (&c->l);
        block_read (c->block, c->sector, c->data);
        c->loading = false;
        rw_writer_unlock (&c->l);
      }
    lock_release (&read_queue_lock);
  }
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
        return NULL;
    }

  if (cache_full || list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      cache_full = true;

      ASSERT (!list_empty (&cache));
      e = list_pop_front (&cache);

      /* TODO: Synchronization. Acquire a reader lock? */
      c = list_entry (e, struct cache_entry, elem);

      /* TODO: Remove prints. */
      int i = 0;
      while (c->loading || c->accessed || c->dirty)
        {
          i++;
          printf ("starting cycle %d\n", i);
          if (c->writing_dirty)
            {
              if (c->accessed)
                c->accessed = false;
              else
                break;
            }
          else
            {
              /* TODO: Acquire a writer lock? */
              if (c->dirty)
                {
                  c->writing_dirty = true;
                  lock_acquire (&dirty_queue_lock);
                  list_push_back (&dirty_queue, &c->d_elem);
                  cond_signal (&dirty_queue_empty, &dirty_queue_lock);
                  lock_release (&dirty_queue_lock);
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
      ASSERT (c != NULL); /* TODO: Graceful failure. */
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

  /* TODO: Should we force read-ahead
     and write-behind to die before proceeding? */

  /* Force read-ahead and write-behind to die. */
  running = false;

  lock_acquire (&read_queue_lock);
  cond_signal (&read_queue_empty, &read_queue_lock);
  lock_release (&read_queue_lock);

  lock_acquire (&dirty_queue_lock);
  cond_signal (&dirty_queue_empty, &dirty_queue_lock);
  lock_release (&dirty_queue_lock);

  /* Now clear the cache. */
  while (list_size (&cache) > 0)
    {
      ASSERT (!list_empty (&cache));
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);
      /* TODO: Is is possible that someone else is touching
         c right now? */
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

/*
void cache_write_periodically (void *aux UNUSED)
{
  thread_current ()->background = true;

  struct list_elem *e;
  struct cache_entry *c;

  while (running)
    {
      ASSERT (false);
      timer_msleep (30000);

      rw_reader_lock (&cache_lock);

      for (e = list_begin (&cache); e != list_end (&cache);
           e = list_next (e))
        {
          c = list_entry (e, struct cache_entry, elem);
          if (c->dirty)
            {
              c->writing_dirty = true;
              thread_create ("write-behind", PRI_DEFAULT, cache_write_dirty, c);
            }
        }

      rw_reader_unlock (&cache_lock);
    }
}
*/

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

      // TODO: read-ahead
    }
}

void cache_write_bytes (struct block *block, block_sector_t sector,
                        int sector_ofs, int chunk_size, void *buffer)
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
