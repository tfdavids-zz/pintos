#include "vm/page.h"

#include <string.h>
#include "lib/stdint.h"
#include "lib/stdbool.h"
#include "lib/debug.h"
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"

static unsigned supp_pt_hash_func (const struct hash_elem *e, void *aux);
static bool supp_pt_less_func (const struct hash_elem *a,
  const struct hash_elem *b, void *aux);
static void supp_pt_free_func(struct hash_elem *e, void *aux);
static struct supp_pte *supp_pte_lookup (struct hash *h, void *address);
static void supp_pte_fetch (struct hash *h, struct supp_pte *e, void *kpage);

// struct for an entry in the supplemental page table
struct supp_pte
  {
    void *address;
    bool writable;

    enum data_loc loc;

    // for pages on swap, we need this
    struct block *block;
    block_sector_t sector;

    // and for pages on disk, we need this
    struct file *file;
    off_t start;

    struct hash_elem hash_elem;
  };

static unsigned
supp_pt_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
  return hash_bytes (&pte->address, sizeof(void *));
}

static bool
supp_pt_less_func (const struct hash_elem *a, const struct hash_elem *b,
  void *aux UNUSED)
{
  struct supp_pte *pte_a = hash_entry (a, struct supp_pte, hash_elem);
  struct supp_pte *pte_b = hash_entry (b, struct supp_pte, hash_elem);
  return hash_bytes (&pte_a->address, sizeof (void *)) <
       hash_bytes (&pte_b->address, sizeof (void *));
}

/* TODO: Verify that this is correct (in particular, verify that
 we should close the file. */
static void
supp_pt_free_func(struct hash_elem *e, void *aux)
{
  struct supp_pte *pte = hash_entry(e, struct supp_pte, hash_elem);
  /*
  if (pte->file)
    {
      file_close (pte->file);
    }
  */
  free (pte);
}

bool
supp_pt_init (struct hash *h)
{
  return hash_init (h, supp_pt_hash_func, supp_pt_less_func, NULL);
}

void
supp_pt_destroy(struct hash *h)
{
  hash_destroy (h, supp_pt_free_func);
}

static struct supp_pte *
supp_pte_lookup (struct hash *h, void *address)
{
  address = pg_round_down (address); // round down to page
  
  struct supp_pte key;
  key.address = address;

  // find the element in our hash table corresponding to this page
  struct hash_elem *e = hash_find (h, &key.hash_elem);

  struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
  return pte;
}

static void
supp_pte_fetch (struct hash *h, struct supp_pte *e, void *kpage)
{
  int i;
  switch (e->loc)
    {
      case DISK:
        file_read_at (e->file, kpage, PGSIZE, e->start);
        break;
      case SWAP:
      	for (i = 0; i < PGSIZE; i += BLOCK_SECTOR_SIZE)
        	block_read (e->block, e->sector + i / BLOCK_SECTOR_SIZE,
        				(char *)kpage + i);
        break;
      case ZEROES:
        memset (kpage, 0, PGSIZE);
        break;
      default:
        // SHOULD NEVER GET HERE
        ASSERT (false);
    }
}

// TODO: verify interface is working for disk and swap pages
bool
page_alloc (struct hash *h, void *upage, enum data_loc loc,
			struct block *block, block_sector_t sector,
			struct file *file, off_t start,
			bool writable)
{
  struct supp_pte *e = malloc (sizeof (struct supp_pte));
  if (!e)
    {
      return false;
    }

  e->address = upage;
  e->loc = loc;
  e->block = block;
  e->sector = sector;
  e->file = file;
  e->start = start;
  e->writable = writable;

  // for debugging: make sure we've set the right variables
  // TODO: probably don't need this in the final version
  switch (e->loc)
    {
      case DISK:
        ASSERT (e->file);
        break;
      case SWAP:
        ASSERT (e->block);
        break;
      case ZEROES:
        ASSERT (!e->file);
        ASSERT (!e->block);
        break;
      default:
        // should never get here
        ASSERT (false);
        break;
    }

  hash_insert (h, &e->hash_elem);
  return true;
}

bool
page_handle_fault (struct hash *h, void *upage)
{
  struct supp_pte *e = supp_pte_lookup (h, upage);
  ASSERT (pagedir_get_page (t->pagedir, upage) == NULL);
  void *kpage = frame_alloc (upage);
  if (!kpage)
    {
      /* TODO: Swapping. */
      ASSERT("false");
    }

  supp_pte_fetch (h, e, kpage);
  return pagedir_set_page (thread_current ()->pagedir,
    upage, kpage, e->writable);
}
