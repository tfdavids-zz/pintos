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

static struct list cache;
static struct lock cache_lock; // used for metadata

struct cache_entry
{
  struct list_elem elem;

  struct block *block;
  block_sector_t sector;
  bool accessed;
  bool loaded;
  bool dirty;
  bool writing_dirty; // if we're in the process of writing to disk
  char data[BLOCK_SECTOR_SIZE];
  struct lock l; // used for working with data[]
};

void cache_write_dirty (struct cache_entry *c);
void cache_read_ahead (struct cache_entry *c);
struct cache_entry *cache_get (struct block *block, block_sector_t sector);
struct cache_entry *cache_evict (void);

void cache_init (void)
{
  lock_init (&cache_lock);
  list_init (&cache);
}

void cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  lock_acquire (&cache_lock);
  
  // check if cache contains block and sector
  struct cache_entry *c = cache_get (block, sector);
  if (c != NULL)
    {
      lock_acquire (&c->l);
      lock_release (&cache_lock);
      memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
      c->accessed = true;
      lock_release (&c->l);
      return;
    }

  // didn't find it, so pop something
  c = cache_evict ();
  
  lock_acquire (&c->l);
  c->block = block;
  c->sector = sector;
  c->loaded = false;
  list_push_back (&cache, &c->elem);
  lock_release (&cache_lock);
  block_read (block, sector, c->data);
  memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
  lock_acquire (&cache_lock);
  c->loaded = true;
  lock_release (&cache_lock);
  struct cache_entry *c_copy = malloc (sizeof (struct cache_entry));
  memcpy (c_copy, c, sizeof (struct cache_entry));
  lock_release (&c->l);
  thread_create ("read-ahead", PRI_MIN, cache_read_ahead, c_copy);
}

void cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  lock_acquire (&cache_lock);

  // check if cache contains block and sector
  struct cache_entry *c = cache_get (block, sector);
  if (c != NULL)
    {
      lock_acquire (&c->l);
      lock_release (&cache_lock);
      memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
      c->dirty = true;
      c->writing_dirty = false;
      c->accessed = true;
      lock_release (&c->l);
      return;
    }

  // didn't find it, so pop something
  c = cache_evict ();
  
  lock_acquire (&c->l);
  c->block = block;
  c->sector = sector;
  c->loaded = false;
  list_push_back (&cache, &c->elem);
  lock_release (&cache_lock);
  memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
  c->dirty = true;
  c->loaded = true;
  lock_release (&c->l);
}

void cache_write_dirty (struct cache_entry *c)
{
  thread_current ()->background = true;
  lock_acquire (&c->l);
  block_write (c->block, c->sector, c->data);
  c->dirty = false;
  c->writing_dirty = false;
  lock_release (&c->l);
}

struct cache_entry *cache_get (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          return c;
        }
    }

  return NULL;
}

struct cache_entry *cache_evict ()
{
  struct cache_entry *c;
  struct list_elem *e;

  if (list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);
    
      while (c->accessed == true || c->dirty == true)
        {
          if (c->writing_dirty)
            {
              // do nothing
            }
          else if (c->dirty && !c->writing_dirty)
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
      lock_init (&c->l);
    }

  return c;
}

void cache_flush (void)
{
  lock_acquire (&cache_lock);

  struct cache_entry *c;
  struct list_elem *e;

  while (list_size (&cache) > 0)
    {
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);
      if (c->dirty)
        {
          lock_acquire (&c->l);
          c->writing_dirty = true;
          block_write (c->block, c->sector, c->data);
          c->dirty = false;
          c->writing_dirty = false;
          lock_release (&c->l);
        }

      free (c);
    }

  lock_release (&cache_lock);
}

void cache_read_ahead (struct cache_entry *c)
{
  thread_current ()->background = true;
  struct block *block = c->block;
  block_sector_t sector = c->sector + 1;
  free (c);

  lock_acquire (&cache_lock);
  
  // check if cache contains block and sector
  c = cache_get (block, sector);
  if (c != NULL)
      return;

  // didn't find it, so pop something
  c = cache_evict ();
  
  lock_acquire (&c->l);
  c->block = block;
  c->sector = sector;
  c->loaded = false;
  list_push_back (&cache, &c->elem);
  lock_release (&cache_lock);
  block_read (block, sector, c->data);
  c->loaded = true;
  lock_release (&c->l);
}
