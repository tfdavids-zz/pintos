/*
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
  char data[BLOCK_SECTOR_SIZE];
  struct lock l; // used for working with data[]
};

void cache_init (void)
{
  lock_init (&cache_lock);
  list_init (&cache);
}

struct cache_entry *cache_eviction_candidate (void)
{
  struct list_elem *e = list_pop_front (&cache);
  struct cache_entry *c = list_entry (e, struct cache_entry, elem);

  while (c->accessed == true)
    {
      c->accessed = false;
      list_push_back (&cache, e);
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);
    }
  
  return c;
}

/* Inserts an element to the cache at a random location without evicting
 * another one.
 *
void cache_insert (struct cache_entry *new)
{
  list_push_back (&cache, &new->elem);
}

size_t num_cached_elements (void)
{
  return list_size (&cache);
}

bool cache_contains (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  lock_acquire (&cache_lock);

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          lock_release (&cache_lock);
          return true;
        }
    }

  lock_release (&cache_lock);
  return false;
}

void read_next (void *aux)
{
  struct cache_entry *curr = (struct cache_entry *) aux;
  if (!cache_contains (curr->block, curr->sector))
    {
      lock_acquire (&cache_lock);
      cache_add (curr->block, curr->sector + 1);
    }
}

// MUST HOLD CACHE_LOCK
void cache_add (struct block *block, block_sector_t sector)
{
  if (num_cached_elements () >= NUM_CACHE_BLOCKS)
    {
      struct cache_entry *old = cache_eviction_candidate ();
      lock_acquire (&old->l);
      old->block = block;
      old->sector = sector;
      list_push_back (&cache, old);
      lock_release (&cache_lock);
      // now we can happily read without blocking everybody else
      block_read (block, sector, old->data);
      lock_release (&old->l);
    }
  else
    {
      struct cache_entry *new = malloc (sizeof (struct cache_entry));
      new->block = block;
      new->sector = sector;
      lock_init (&new->l);
      lock_acquire (&new->l);
      list_push_back (&cache, new);
      lock_release (&cache_lock);
      block_read (block, sector, new->data);
      lock_release (&new->l);
    }
}

void cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  struct list_elem *e;
  struct cache_entry *c;

  lock_acquire (&cache_lock);
  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      lock_acquire (&c->l);
      if (c->block == block && c->sector == sector)
        {
          c->accessed = true;
          memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
          lock_release (&c->l);
          lock_release (&cache_lock);
          return;
        }
      lock_release (&c->l);
    }

  // if we're here, our cache doesn't contain the desired block

  cache_add (block, sector);

  // read ahead
  thread_create ("read-ahead", PRI_DEFAULT, read_next, c);

}

void cache_write_dirty (struct block *block, block_sector_t sector)
{
  return; // TODO
}
*/
//////////////
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
  char data[BLOCK_SECTOR_SIZE];
  struct lock l; // used for working with data[]
};

void cache_init (void)
{
  lock_init (&cache_lock);
  list_init (&cache);
}

void cache_read (struct block *block, block_sector_t sector, void *buffer)
{
  lock_acquire (&cache_lock);
  
  // check if cache contains block and sector
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          // we've found it -- act on it!
          lock_acquire (&c->l);
          lock_release (&cache_lock);
          memcpy (buffer, c->data, BLOCK_SECTOR_SIZE);
          c->accessed = true;
          lock_release (&c->l);
          return;
        }
    }

  // didn't find it, so pop something
  if (list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);
    
      while (c->accessed == true)
        {
          c->accessed = false;
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
  lock_release (&c->l);
}

void cache_write (struct block *block, block_sector_t sector, const void *buffer)
{
  lock_acquire (&cache_lock);

  // check if cache contains block and sector
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          // we've found it -- act on it!
          lock_acquire (&c->l);
          lock_release (&cache_lock);
          memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
          block_write (block, sector, c->data); // TODO: write-behind
          c->accessed = true;
          c->dirty = true;
          lock_release (&c->l);
          return;
        }
    }

  // didn't find it, so pop something
  if (list_size (&cache) >= NUM_CACHE_BLOCKS)
    {
      e = list_pop_front (&cache);
      c = list_entry (e, struct cache_entry, elem);
    
      while (c->accessed == true)
        {
          c->accessed = false;
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
  
  lock_acquire (&c->l);
  c->block = block;
  c->sector = sector;
  c->loaded = false;
  list_push_back (&cache, &c->elem);
  lock_release (&cache_lock);
  memcpy (c->data, buffer, BLOCK_SECTOR_SIZE);
  block_write (block, sector, c->data); // TODO: write-behind
  lock_acquire (&cache_lock);
  c->loaded = true;
  lock_release (&cache_lock);
  lock_release (&c->l);
}
