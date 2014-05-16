#include <bitmap.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>

#include "devices/block.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"

static struct block *swap;
static struct bitmap *swap_slots;
static size_t sectors_per_page = BLOCK_SECTOR_SIZE / PGSIZE;
size_t num_swap_slots;

void swap_init (void)
{
  swap = block_get_role (BLOCK_SWAP);
  num_swap_slots = block_size (swap) * sectors_per_page;
  swap_slots = bitmap_create (num_swap_slots);
  bitmap_set_all (swap_slots, false);
}



size_t swap_writes_page (void *upage)
{
  size_t free_slot_index = bitmap_scan (swap_slots, 0, 1, false);
  if (free_slot_index == BITMAP_ERROR)
    {
      /* TODO: decide what to do when swap is full */
    }

  size_t i = 0;
  for (; i < sectors_per_page; i++)
    {
      block_write (swap,
                   free_slot_index * sectors_per_page + i,
                   upage + i * BLOCK_SECTOR_SIZE);

    }
  bitmap_set (swap_slots, free_slot_index, true);

  return free_slot_index;
}


bool swap_load_page (size_t slot_index, void *upage)
{
  if (slot_index > num_swap_slots ||
      !bitmap_test(swap_slots, slot_index))
    {
      return false;
    }
  size_t i = 0;
  for (; i < sectors_per_page; i++)
    {
      block_read (swap,
                  slot_index * sectors_per_page + i,
                  upage + i * BLOCK_SECTOR_SIZE);
    }
  bitmap_set (swap_slots, slot_index, false);

  return true;
}
