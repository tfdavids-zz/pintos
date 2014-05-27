#include "filesys/cache.h"
#include <list.h>
#include "lib/stdbool.h"
#include "threads/malloc.h"
#include "devices/block.h"

#define NUM_CACHE_BLOCKS 64

/* From cache.h:

static void cache_add (struct block *block, block_sector_t sector);

*/

static struct list cache;

struct cache_entry
{
  struct list_elem elem;

  struct block *block;
  block_sector_t sector;
  char data[BLOCK_SECTOR_SIZE];
};

void cache_init (void)
{
  list_init (&cache);
}

void cache_evict (void)
{
  return; // TODO
}

int num_cached_elements (void)
{
  return 0; // TODO
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
        return c->data;
    }
  return NULL;
}

void cache_add (struct block *block, block_sector_t sector)
{
  if (cache_contains (block, sector))
    return;
  if (num_cached_elements () >= NUM_CACHE_BLOCKS)
    cache_evict ();

  struct cache_entry *e = malloc (sizeof (struct cache_entry));
  e->block = block;
  e->sector = sector;
  block_read (block, sector, &e->data);

  list_push_front (&cache, &e->elem);
}

void cache_write_dirty (struct block *block, block_sector_t sector)
{
  return; // TODO
}

