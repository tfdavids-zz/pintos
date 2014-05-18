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
#include "vm/swap.h"

static unsigned supp_pt_hash_func (const struct hash_elem *e, void *aux);
static bool supp_pt_less_func (const struct hash_elem *a,
  const struct hash_elem *b, void *aux);
static void supp_pt_free_func (struct hash_elem *e, void *aux);
static struct supp_pte *supp_pt_lookup (struct hash *h, const void *address);
static void supp_pt_fetch (struct supp_pte *e, void *kpage);
static struct supp_pte *supp_pt_page_alloc (struct hash *h, void *upage,
  enum data_loc loc, struct file *file, off_t start, size_t bytes,
  mapid_t mapid, bool writable);

// struct for an entry in the supplemental page table
struct supp_pte
  {
    const void *address;
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
supp_pt_free_func (struct hash_elem *e, void *aux)
{
  struct thread *t = thread_current ();
  struct supp_pte *supp_pte = hash_entry (e, struct supp_pte, hash_elem);
  void *upage = supp_pte->address;

  /* Write if dirty! */
  if (supp_pte->mapping >= 0)
    {
      if (pagedir_is_dirty (t->pagedir, upage))
        {
          lock_acquire (&filesys_lock);
          file_write_at (supp_pte->file, upage,
            supp_pte->bytes, supp_pte->start);
          lock_release (&filesys_lock);
        }
    }

  /* Free whatever memory that this page occupied. */
  /* TODO: Synchronization. */
  switch (supp_pte->loc)
    {
      void *kpage;
      case PRESENT:
        kpage = pagedir_get_page (t->pagedir, upage);
        ASSERT (kpage != NULL); 
        pagedir_clear_page (t->pagedir, upage); /* Clear the mapping. */
        frame_free (kpage);
        break;
      case SWAP:
        /* TODO: Free swap. */
        break;
      case DISK:
      case ZEROES:
        /* TODO: What should be here, if anything? */
        break;
      default:
        NOT_REACHED ();
    }


  /* Free the entry itself. */
  free (supp_pte);
}

bool
supp_pt_init (struct hash *h)
{
  return hash_init (h, supp_pt_hash_func, supp_pt_less_func, NULL);
}

void
supp_pt_destroy (struct hash *h)
{
  hash_destroy (h, supp_pt_free_func);
}

static struct supp_pte *
supp_pt_lookup (struct hash *h, const void *address)
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

bool
supp_pt_page_alloc_file (struct hash *h, void *upage,
  struct file *file, off_t start, size_t bytes, mapid_t mapid, bool writable)
{
  return supp_pt_page_alloc (h, upage, DISK, file, start, bytes,
    mapid, writable) != NULL;
}

bool
supp_pt_page_calloc (struct hash *h, void *upage, bool writable)
{
  return supp_pt_page_alloc (h, upage, ZEROES,
    NULL, 0, 0, -1, writable) != NULL;
}

static struct supp_pte *
supp_pt_page_alloc (struct hash *h, void *upage, enum data_loc loc,
  struct file *file, off_t start, size_t bytes,
  mapid_t mapid, bool writable)
{
  struct supp_pte *e = malloc (sizeof (struct supp_pte));
  if (!e)
    {
      return NULL;
    }

  e->address = upage;
  e->loc = loc;
  e->file = file;
  e->start = start;
  e->bytes = bytes;
  e->mapping = mapid;
  e->writable = writable;

  /* There should not already exist a supp_pte
     for this upage in the supplementary page table. */
  if (hash_insert (h, &e->hash_elem) != NULL)
    {
      free (e);
      return NULL;
    }
  return e;
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
  if (e == NULL)
    {
      return false;
    }

  struct thread *t = thread_current ();
  ASSERT (pagedir_get_page (t->pagedir, upage) == NULL);
  void *kpage = frame_alloc (upage);
  if (!kpage)
    {
      /* TODO: Swapping. */
      ASSERT (false);
    }

  /* TODO: Synchronization. */
  supp_pt_fetch (e, kpage);
  if (pagedir_set_page (t->pagedir,
    upage, kpage, e->writable))
    {
      e->loc = PRESENT;
      return true;
    }
  else
    {
      return false;
    }
}

bool
supp_pt_page_free (struct hash *h, void *upage)
{
  struct supp_pte key;
  key.address = upage;
  struct supp_pte *supp_pte =
    hash_entry (hash_delete (h, &key.hash_elem), struct supp_pte, hash_elem);

  if (supp_pte)
    {
      supp_pt_free_func (&supp_pte->hash_elem, NULL);
      return true;
    }
  return false;
}

bool
supp_pt_munmap (struct hash *h, void *first_mmap_page)
{
  struct supp_pte *supp_curr = supp_pt_lookup (h, first_mmap_page);
  if (supp_curr == NULL || supp_curr->mapping != (mapid_t)first_mmap_page)
    {
      return false;
    }
  struct file *file = supp_curr->file;

  /* Approach:
      1) Calcualte total num pages.
      2) Start at page i
      3) delete and free page i
      4) i++
      5) go to 2) unless k >= num pages, then done.
   */
  lock_acquire (&filesys_lock);
  size_t num_pages = file_length (supp_curr->file) / PGSIZE + 1;
  lock_release (&filesys_lock);

  size_t i;
  for (i = 0; i < num_pages; i++)
    {
      /* TODO: Remove assert wrapping. */
      ASSERT(supp_pt_page_free (h, (void *)
        ((uintptr_t)first_mmap_page + i * PGSIZE)));
    }
  
  lock_acquire (&filesys_lock);
  file_close (file);
  lock_release (&filesys_lock);
  return true;
}
