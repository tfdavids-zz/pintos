#include "vm/page.h"

#include <string.h>
#include "lib/stdint.h"
#include "lib/stdbool.h"
#include "lib/debug.h"
#include "lib/kernel/hash.h"
#include "lib/user/syscall.h"
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"

static unsigned supp_pt_hash_func (const struct hash_elem *e, void *aux);
static bool supp_pt_less_func (const struct hash_elem *a,
  const struct hash_elem *b, void *aux);
static void supp_pt_free_func(struct hash_elem *e, void *aux);
static struct supp_pte *supp_pt_lookup (struct hash *h, void *address);
static void supp_pt_fetch (struct supp_pte *e, void *kpage);
static bool supp_pt_page_alloc (struct hash *h, void *upage, enum data_loc loc,
  struct file *file, off_t start, size_t bytes, bool writable);

// struct for an entry in the supplemental page table
struct supp_pte
  {
    void *address;
    bool writable;

    enum data_loc loc;

    // for pages on swap, we need this
    size_t swap_index;

    // and for pages on disk, we need this
    struct file *file; /* The file backing this page. */
    off_t start;  /* The offset in the file at which the data begin. */
    size_t bytes; /* The number of bytes to copy from the file. */
    mapid_t mapping; /* Negative if this page was not mmaped; otherwise,
                        the id of the mapping. */

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

/* TODO: Verify that this frees everything that should be freed. */
/* TODO: Will there ever be a case where the file is not closed by
   the process upon exit? */
static void
supp_pt_free_func(struct hash_elem *e, void *aux)
{
  struct supp_pte *pte = hash_entry(e, struct supp_pte, hash_elem);
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
supp_pt_lookup (struct hash *h, void *address)
{
  address = pg_round_down (address); // round down to page
  
  struct supp_pte key;
  key.address = address;

  // find the element in our hash table corresponding to this page
  struct hash_elem *e = hash_find (h, &key.hash_elem);
  if (!e)
    {
      return NULL;
    }

  struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
  return pte;
}

static void
supp_pt_fetch (struct supp_pte *e, void *kpage)
{
  switch (e->loc)
    {
      case DISK:
        lock_acquire (&filesys_lock);
        file_read_at (e->file, kpage, e->bytes, e->start);
        lock_release (&filesys_lock);
        memset ((uint8_t *)kpage + e->bytes, 0, PGSIZE - e->bytes);
        break;
      case SWAP:
        swap_load_page (e->swap_index, kpage);
        break;
      case ZEROES:
        memset (kpage, 0, PGSIZE);
        break;
      default:
        NOT_REACHED () ;
    }
}

// TODO: verify interface is working for disk and swap pages
bool
supp_pt_page_alloc_file (struct hash *h, void *upage,
  struct file *file, off_t start, size_t bytes, bool writable)
{
  return supp_pt_page_alloc (h, upage, DISK, file, start, bytes, writable);
}

bool
supp_pt_page_calloc (struct hash *h, void *upage, bool writable)
{
  return supp_pt_page_alloc (h, upage, ZEROES, NULL, 0, 0, writable);
}

static bool
supp_pt_page_alloc (struct hash *h, void *upage, enum data_loc loc,
  struct file *file, off_t start, size_t bytes, bool writable)
{
  struct supp_pte *e = malloc (sizeof (struct supp_pte));
  if (!e)
    {
      return false;
    }

  e->address = upage;
  e->loc = loc;
  e->file = file;
  e->start = start;
  e->bytes = bytes;
  e->writable = writable;

  /* There should not already exist a supp_pte
     for this upage in the supplementary page table. */
  return hash_insert (h, &e->hash_elem) == NULL;
}

bool
supp_pt_page_exists (struct hash *h, const void *upage)
{
  return supp_pt_lookup (h, upage) != NULL;
}

bool
page_handle_fault (struct hash *h, void *upage)
{
  struct supp_pte *e = supp_pt_lookup (h, upage);
  struct thread *t = thread_current ();
  ASSERT (pagedir_get_page (t->pagedir, upage) == NULL);
  void *kpage = frame_alloc (upage);
  if (!kpage)
    {
      /* TODO: Swapping. */
      ASSERT (false);
    }

  supp_pt_fetch (e, kpage);
  return pagedir_set_page (t->pagedir,
    upage, kpage, e->writable);
}

bool
supp_pt_page_free (struct hash *h, void *upage)
{
  struct supp_pte key;
  key.address = upage;
  struct hash_elem *e = hash_delete (h, &key.hash_elem);
  if (e)
    {
      free (hash_entry (e, struct supp_pte, hash_elem));
      return true;
    }
  return false;
}
