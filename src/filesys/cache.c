#include "filesys/cache.h"
#include <string.h>
#include <list.h>
#include <stdio.h>
#include "lib/stdbool.h"
#include "threads/malloc.h"
#include "devices/block.h"

#define NUM_CACHE_BLOCKS 64

/* From cache.h:

static void cache_add (struct block *block, block_sector_t sector);

*/

static struct list cache;

static struct list_elem *clock_position;

struct cache_entry
{
  struct list_elem elem;

  struct block *block;
  block_sector_t sector;
  bool accessed;
  char data[BLOCK_SECTOR_SIZE];
};

void cache_init (void)
{
  list_init (&cache);
}

void cache_replace (struct cache_entry *old, struct cache_entry *new)
{
  old->block = new->block;
  old->sector = new->sector;
  memcpy (new->data, old->data, BLOCK_SECTOR_SIZE);
  old->accessed = false;
  free (new);
}

struct cache_entry *cache_eviction_candidate (void)
{
  struct list_elem *e = clock_position;
  struct cache_entry *c = list_entry (e, struct cache_entry, elem);

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
  while (c->accessed == true)
    {
      c->accessed = false;
      if (e == list_end (&cache))
        e = list_begin (&cache);
      else
        e = list_next (e);

      c = list_entry (e, struct cache_entry, elem);
    }
  
  if (e == list_end (&cache))
    e = list_begin (&cache);
  else
    e = list_next (e);

  return c;
}

/* Inserts an element to the cache at a random location without evicting
 * another one.
 */
void cache_insert (struct cache_entry *new)
{
  list_push_back (&cache, &new->elem);
  if (clock_position == NULL)
    clock_position = &new->elem;
}

size_t num_cached_elements (void)
{
  return list_size (&cache);
}

bool cache_contains (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        return true;
    }
  return false;
}

void *cache_get (struct block *block, block_sector_t sector)
{
  struct list_elem *e;
  struct cache_entry *c;

  for (e = list_begin (&cache); e != list_end (&cache);
       e = list_next (e))
    {
      c = list_entry (e, struct cache_entry, elem);
      if (c->block == block && c->sector == sector)
        {
          c->accessed = true;
          return c->data;
        }
    }
  return NULL;
}

void cache_add (struct block *block, block_sector_t sector)
{
  if (cache_contains (block, sector))
    return;

  struct cache_entry *e = malloc (sizeof (struct cache_entry));
  e->block = block;
  e->sector = sector;
  block_read (block, sector, &e->data);

  if (num_cached_elements () >= NUM_CACHE_BLOCKS)
    {
      struct cache_entry *old = cache_eviction_candidate ();
      cache_replace (old, e);
    }
  else
    cache_insert (e);
}

void cache_write_dirty (struct block *block, block_sector_t sector)
{
  return; // TODO
}

