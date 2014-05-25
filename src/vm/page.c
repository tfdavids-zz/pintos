#include "vm/page.h"

#include <string.h>
#include "lib/stdint.h"
#include "lib/stdbool.h"
#include "lib/debug.h"
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
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
static void supp_pt_fetch (struct supp_pte *e, void *kpage);
static struct supp_pte *supp_pt_page_alloc (struct supp_pt *supp_pt,
  void *upage, enum data_loc loc, struct file *file, off_t start,
  size_t bytes, mapid_t mapid, bool writable);

/* Initializes the supplementary page table. */
bool
supp_pt_init (struct supp_pt *supp_pt)
{
  lock_init (&supp_pt->lock);
  cond_init (&supp_pt->done_updating);
  supp_pt->num_updating = 0;
  return hash_init (&supp_pt->h, supp_pt_hash_func, supp_pt_less_func, NULL);
}

/* Creates a supplementary page table entry for a page of data to be brought
   in from a file, starting at offset 'start' and extending for 'bytes'
   number of bytes. The page will be lazy-loaded.

   If the page is a memory mapping, then mapid should be a valid mapping ID;
   otherwise, it should be equal to MAP_FAILED. In particular, a memory mapping
   starting at address 'vaddr' should have a map ID of '(void *)vaddr'.

   Returns true if the entry was successfully inserted into the table; returns
   false if an entry with the same upage was already in the table, or if
   memory allocation failed.*/
bool
supp_pt_page_alloc_file (struct supp_pt *supp_pt, void *upage,
  struct file *file, off_t start, size_t bytes, mapid_t mapid, bool writable)
{
  return supp_pt_page_alloc (supp_pt, upage, DISK, file, start, bytes,
    mapid, writable) != NULL;
}

/* Creates a supplementary page table entry for a page of zeroed data (e.g.
   BSS). The page will be lazy-loaded.

   Returns true if the entry was successfully inserted into the table; returns
   false if an entry with the same upage was already in the table, or if
   memory allocation failed.*/
bool
supp_pt_page_calloc (struct supp_pt *supp_pt, void *upage, bool writable)
{
  return supp_pt_page_alloc (supp_pt, upage, ZEROES,
    NULL, 0, 0, -1, writable) != NULL;
}

/* Creates a supplementary page table entry, initializes its fields, and inserts
   it into the table.

   Returns true if the entry was successfully inserted into the table; returns
   false if an entry with the same upage was already in the table, or if
   memory allocation failed.*/
static struct supp_pte *
supp_pt_page_alloc (struct supp_pt *supp_pt, void *upage, enum data_loc loc,
  struct file *file, off_t start, size_t bytes,
  mapid_t mapid, bool writable)
{
  struct supp_pte *e = malloc (sizeof (struct supp_pte));
  if (!e)
    {
      return NULL;
    }

  e->upage = upage;
  e->loc = loc;
  e->file = file;
  e->start = start;
  e->bytes = bytes;
  e->mapping = mapid;
  e->writable = writable;
  e->pinned = false;
  e->being_evicted = false;
  lock_init (&e->l);
  cond_init (&e->done_evicting);

  /* There should not already exist a supp_pte
     for this upage in the supplementary page table.
   */
  lock_acquire (&supp_pt->lock);
  if (hash_insert (&supp_pt->h, &e->hash_elem) != NULL)
    {
      free (e);
      e = NULL;
    }
  lock_release (&supp_pt->lock);
  return e;
}


/* Handles a page fault that occured upon a valid user virtual
   page by loading the page into memory. The page is pinned for
   the duration of the handling process.

   Returns true if the page was successfully loaded into memory,
   false otherwise.*/
bool
supp_pt_handle_fault (struct supp_pt *supp_pt, void *upage)
{
  ASSERT (is_user_vaddr (upage));
  struct supp_pte *e = supp_pt_lookup (supp_pt, upage);
  bool success = false;
  if (e == NULL)
    {
      return false;
    }

  e->pinned = true;
  success = supp_pt_force_load (e);
  e->pinned = false;

  return success;
}

/* Loads the user page associated with the supplied
   supp_pte into memory and clears its dirty bit.

   NB: This function is synchronized with eviction.
   Returns true if and only if the page was successfully loaded. */
bool
supp_pt_force_load (struct supp_pte *supp_pte)
{
  struct thread *t = thread_current ();

  /* Do not load the page if it is already in memory. */
  if (pagedir_get_page(t->pagedir, supp_pte->upage))
    {
      return true;
    }

  /* Retrieve the kernel virtual address to which this
     user page should be aliased. */
  void *kpage = frame_alloc (supp_pte->upage);
  if (!kpage)
    {
      return false;
    }

  /* Do not fetch the page while eviction is in process. */
  lock_acquire (&supp_pte->l);
  while (supp_pte->being_evicted)
    {
      cond_wait (&supp_pte->done_evicting, &supp_pte->l);
    }
  lock_release (&supp_pte->l);

  /* Fetch the page and update the page directory. */
  supp_pt_fetch (supp_pte, kpage);
  pagedir_set_dirty (t->pagedir, supp_pte->upage, false);
  if (pagedir_set_page (t->pagedir,
    supp_pte->upage, kpage, supp_pte->writable))
    {
      supp_pte->loc = MEMORY;
      return true;
    }
  else
    {
      return false;
    }
}

/* Grow the stack if necessary.

   If the provided user virtual address appears to be a
   a stack reference, create a supplementary page table
   entry for the address' page (if one does not already
   exist.

   Returns false if an attempt to grow the stack failed;
   returns true if either the attempt succeeded, or if no
   attempt was made. */
bool
supp_pt_grow_stack_if_necessary (struct supp_pt *supp_pt,
  void *esp, void *addr)
{
  void *upage = pg_round_down (addr);
  if (is_user_stack_vaddr (addr, esp) &&
    supp_pt_lookup (supp_pt, upage) == NULL)
    {
      return supp_pt_page_calloc (supp_pt, upage, true);
    }
  return true;
}

/* Given a supplementary page table and a user virtual address,
   return the corresponding supplementary page table entry if
   it exists, NULL otherwise. */
struct supp_pte *
supp_pt_lookup (struct supp_pt *supp_pt, void *addr)
{
  void *upage = pg_round_down (addr);
  
  /* Find the corresponding supp_pte in our table. */
  struct supp_pte key;
  key.upage = upage;
  struct hash_elem *e = hash_find (&supp_pt->h, &key.hash_elem);
  if (!e)
    {
      return NULL;
    }

  struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
  return pte;
}

/* Returns true if the provided mapid_t appears to be
   a valid ID.

   NB: A return value of true does not guarantee that
       there exists a mapping in the system with the
       supplied ID. */
bool
supp_pt_is_valid_mapping (mapid_t mapping)
{
  return mapping > 0 && is_user_vaddr ((void *)mapping);
}

/* Given a supplementary page table entry, writes
   the contents of its page to disk (if said page is a memory
   mapping of a file).*/
void
supp_pt_write_if_mapping (struct supp_pte *supp_pte)
{
  if (supp_pt_is_valid_mapping (supp_pte->mapping))
    {
      lock_acquire (&filesys_lock);
      file_write_at (supp_pte->file, supp_pte->upage,
        supp_pte->bytes, supp_pte->start);
      lock_release (&filesys_lock);
    }
}


/* Free resources owned by a struct supp_pte, including any data
   it may have in swap, and free the supp_pte itself.

   Precondition: The supp_pte's user virtual page is not mapped --
                 that is, the virtual page should not reside in memory. */
static void
supp_pt_free_func (struct hash_elem *e, void *aux)
{
  struct thread *t = thread_current ();
  struct supp_pte *supp_pte = hash_entry (e, struct supp_pte, hash_elem);
  void *upage = supp_pte->upage;

  /* Free whatever memory that this page occupied. */
  ASSERT (!pagedir_get_page (t->pagedir, upage));
  if (supp_pte->loc == SWAP)
    {
      swap_free (supp_pte->swap_slot_index);
    }

  /* Free the entry itself. */
  free (supp_pte);
}

/* Free all resources owned by both the provided upage and
   the supp_pte corresponding to the upage.

   Precondition: There must exist an entry for upage in supp_pt. */
void
supp_pt_page_free (struct supp_pt *supp_pt, void *upage)
{
  /* Retrieve the corresponding entry. */
  struct supp_pte *supp_pte = supp_pt_lookup (supp_pt, upage);
  ASSERT (supp_pte != NULL)

  /* If there exists a frame for the entry, free it and its resources
     before proceeding.

     By deleting the frame -- and removing it from the frame table --
     before deleting the corresponding supp_pte, we avoid
     a race in which the system might free a supp_pte and simultaneously
     attempt to evict its frame. */
  void *kpage = pagedir_get_page (thread_current ()->pagedir, upage);
  if (kpage)
    {
      frame_free (kpage);
    }

  /* Remove the supp_pte entry from the supp_pt table. */
  struct supp_pte key;
  key.upage = upage;
  lock_acquire (&supp_pt->lock);
  hash_delete (&supp_pt->h, &key.hash_elem);
  lock_release (&supp_pt->lock);

  /* Free the entry and any backing store data for its upage. */
  supp_pt_free_func (&supp_pte->hash_elem, NULL);
}

/* Unmap a file by removing and freeing its frames and its
   supplementary page table entries.

   Returns true if the file was successfully unmapped, false
   otherwise (e.g. if a supp_pte entry did not exist for the mapping). */
bool
supp_pt_munmap (struct supp_pt *supp_pt, void *first_mmap_page)
{
  struct supp_pte *supp_pte = supp_pt_lookup (supp_pt, first_mmap_page);
  if (supp_pte == NULL || supp_pte->mapping != (mapid_t)first_mmap_page)
    {
      return false;
    }
  struct file *file = supp_pte->file;

  lock_acquire (&filesys_lock);
  size_t num_pages = pg_range_num (file_length (supp_pte->file));
  lock_release (&filesys_lock);

  size_t i;
  for (i = 0; i < num_pages; i++)
    {
      supp_pt_page_free (supp_pt, (void *)
        ((uintptr_t)first_mmap_page + i * PGSIZE));
    }

  lock_acquire (&filesys_lock);
  file_close (file);
  lock_release (&filesys_lock);
  return true;
}

/* Destroys the supplementary page table by invoking
   hash destroy.

   NB: This function does NOT free frames that this
       thread may own; frames should be freed before
       invoking destroy.
   NB: This function may block, since it ensures that
       no other threads are modifying the table before
       proceeding. */
void
supp_pt_destroy (struct supp_pt *supp_pt)
{
  lock_acquire (&supp_pt->lock);
  while (supp_pt->num_updating > 0)
    {
      cond_wait (&supp_pt->done_updating, &supp_pt->lock);
    }
  hash_destroy (&supp_pt->h, supp_pt_free_func);
  lock_release (&supp_pt->lock);
}

/* Fetch data for a user page that is not resident in memory
   into the kernel page it is aliased into.

   Precondition: The page must not be in memory. */
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
        if (!swap_load_page (e->swap_slot_index, kpage))
          PANIC ("Unable to load page from swap.");
        break;
      case ZEROES:
        memset (kpage, 0, PGSIZE);
        break;
      default:
        NOT_REACHED ();
    }
}

/* Returns the hash code for a struct supp_pte; the hash function
   is computed on the supp_pte's user virtual page. */
static unsigned
supp_pt_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
  return hash_bytes (&pte->upage, sizeof(void *));
}

/* Compares two struct supp_pte's. Returns true if and only if the
   the supp_pte corresponding to 'a' has a user virtual page that
   is less than that of the supp_pte corresponding to 'b'. */
static bool
supp_pt_less_func (const struct hash_elem *a, const struct hash_elem *b,
  void *aux UNUSED)
{
  struct supp_pte *pte_a = hash_entry (a, struct supp_pte, hash_elem);
  struct supp_pte *pte_b = hash_entry (b, struct supp_pte, hash_elem);
  return hash_bytes (&pte_a->upage, sizeof (void *)) <
       hash_bytes (&pte_b->upage, sizeof (void *));
}
