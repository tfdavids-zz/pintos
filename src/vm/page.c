#include "vm/page.h"

#include <string.h>
#include "lib/stdint.h"
#include "lib/stdbool.h"
#include "lib/debug.h"
#include "lib/kernel/hash.h"
#include "filesys/off_t.h"
#include "filesys/file.h"
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

static unsigned
supp_pt_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
  return hash_bytes (&pte->upage, sizeof(void *));
}

static bool
supp_pt_less_func (const struct hash_elem *a, const struct hash_elem *b,
  void *aux UNUSED)
{
  struct supp_pte *pte_a = hash_entry (a, struct supp_pte, hash_elem);
  struct supp_pte *pte_b = hash_entry (b, struct supp_pte, hash_elem);
  return hash_bytes (&pte_a->upage, sizeof (void *)) <
       hash_bytes (&pte_b->upage, sizeof (void *));
}

/* TODO: Verify that this frees everything that should be freed. */
/* NB: This function does NOT free resources collected from the user-pool. */
static void
supp_pt_free_func (struct hash_elem *e, void *aux)
{
  struct thread *t = thread_current ();
  struct supp_pte *supp_pte = hash_entry (e, struct supp_pte, hash_elem);
  void *upage = supp_pte->upage;

  /* Free whatever memory that this page occupied. */
  /* TODO: Synchronization. */
  ASSERT (!pagedir_get_page (t->pagedir, upage));
  if (supp_pte->loc == SWAP)
    {
      /* TODO: Free swap. */
    }


  /* Free the entry itself. */
  /* TODO: I can't do this while I'm being evicted! */
  free (supp_pte);
}

bool
supp_pt_init (struct supp_pt *supp_pt)
{
  lock_init (&supp_pt->lock);
  cond_init (&supp_pt->done_updating);
  supp_pt->num_updating = 0;
  return hash_init (&supp_pt->h, supp_pt_hash_func, supp_pt_less_func, NULL);
}

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

struct supp_pte *
supp_pt_lookup (struct supp_pt *supp_pt, void *upage)
{
  upage = pg_round_down (upage); // round down to page
  
  struct supp_pte key;
  key.upage = upage;

  // find the element in our hash table corresponding to this page
  struct hash_elem *e = hash_find (&supp_pt->h, &key.hash_elem);
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
        /* TODO: Read loop? */
        file_read_at (e->file, kpage, e->bytes, e->start);
        lock_release (&filesys_lock);
        memset ((uint8_t *)kpage + e->bytes, 0, PGSIZE - e->bytes);
        break;
      case SWAP:
        if (!swap_load_page (e->swap_slot_index, kpage))
          PANIC ("Unable to load page from swap");
        break;
      case ZEROES:
        memset (kpage, 0, PGSIZE);
        break;
      default:
        NOT_REACHED ();
    }
}

bool
supp_pt_page_alloc_file (struct supp_pt *supp_pt, void *upage,
  struct file *file, off_t start, size_t bytes, mapid_t mapid, bool writable)
{
  return supp_pt_page_alloc (supp_pt, upage, DISK, file, start, bytes,
    mapid, writable) != NULL;
}

bool
supp_pt_page_calloc (struct supp_pt *supp_pt, void *upage, bool writable)
{
  return supp_pt_page_alloc (supp_pt, upage, ZEROES,
    NULL, 0, 0, -1, writable) != NULL;
}

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
     TODO: NB: Other process must acquire lock if lookup
           would conflict with insertion.
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

/* Writes the contents of the mapping to disk. */
void
supp_pt_write (struct supp_pte *supp_pte)
{
  if (supp_pt_is_valid_mapping (supp_pte->mapping))
    {
      lock_acquire (&filesys_lock);
      file_write_at (supp_pte->file, supp_pte->upage,
        supp_pte->bytes, supp_pte->start);
      lock_release (&filesys_lock);
    }
}

bool
supp_pt_page_exists (struct supp_pt *supp_pt, void *upage)
{
  return supp_pt_lookup (supp_pt, upage) != NULL;
}

bool
page_handle_fault (struct supp_pt *supp_pt, void *upage)
{
  ASSERT (is_user_vaddr (upage));
  struct supp_pte *e = supp_pt_lookup (supp_pt, upage);
  bool success = false;
  if (e == NULL)
    {
      return false;
    }

  e->pinned = true;
  success = page_force_load (e);
  e->pinned = false;

  return success;
}

bool
page_force_load (struct supp_pte *e)
{
  struct thread *t = thread_current ();
  /* Do not load if already in memory */
  if (pagedir_get_page(t->pagedir, e->upage))
    return true;

  void *kpage = frame_alloc (e->upage);
  if (!kpage)
    {
      ASSERT (false);
    }

  lock_acquire (&e->l);
  while (e->being_evicted)
    {
      cond_wait (&e->done_evicting, &e->l);
    }
  lock_release (&e->l);

  supp_pt_fetch (e, kpage);

  pagedir_set_dirty (t->pagedir, e->upage, false);
  if (pagedir_set_page (t->pagedir,
    e->upage, kpage, e->writable))
    {
      return true;
    }
  else
    {
      return false;
    }
}

void
supp_pt_page_free (struct supp_pt *supp_pt, void *upage)
{
  /* Retrieve the corresponding entry. */
  struct supp_pte *supp_pte = supp_pt_lookup (supp_pt, upage);
  ASSERT (supp_pte != NULL)

  /* If there exists a frame for the entry, delete it before proceeding. */
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

  /* Free the entry and any backing store data for upage. */
  /* TODO: Only free data if frame_free didn't free anything. */
  supp_pt_free_func (&supp_pte->hash_elem, NULL);
}

bool
supp_pt_munmap (struct supp_pt *supp_pt, void *first_mmap_page)
{
  struct supp_pte *supp_curr = supp_pt_lookup (supp_pt, first_mmap_page);
  if (supp_curr == NULL || supp_curr->mapping != (mapid_t)first_mmap_page)
    {
      return false;
    }
  struct file *file = supp_curr->file;

  lock_acquire (&filesys_lock);
  size_t num_pages = pg_range_num (file_length (supp_curr->file));
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

bool
supp_pt_is_valid_mapping (mapid_t mapping)
{
  return mapping > 0 && is_user_vaddr ((void *)mapping);
}
