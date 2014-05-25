#include "vm/frame.h"

#include <stdio.h>
#include <string.h>

#include "lib/stdint.h"
#include "lib/debug.h"
#include "lib/kernel/list.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"

static struct list ftable;      /* List of frames in the system. */
static struct lock ftable_lock; /* Lock to synchronize access to ftable. */

/* A struct frame represents a (upage, kpage, thread) tuple where
   thread is a (living) thread in the system whose page table contains
   a mapping between the user virtual page upage and the kernel virtual
   page kpage. */
struct frame
  {
    void *kpage;           /* A kernel virtual page. */
    void *upage;           /* The user virtual page aliased to kpage. */
    struct list_elem elem; /* The list_elem to hook the frame into the
                              frame table. */
    struct thread *t;      /* The thread to which the (upage, kpage) pair
                              belongs. */
  };

/* Initializes the frame table. */
void
frame_table_init (void)
{
  list_init (&ftable);
  lock_init (&ftable_lock);
}

/* Allocate a frame.

   Serves as a wrapper for palloc_get_page (PAL_USER).

   Allocates a frame for the supplied user page by:
     1) Fetching a kernel page to which the user page
        will be aliased.
     2) Creating a struct frame for (upage, kpage, thread_current ());
     3) Adding the struct frame to the frame table.

  Step 2) may require eviction of an existing frame.

  Returns the address of the kpage, or NULL if
  the allocation failed. */
void *
frame_alloc (void *upage)
{
  void *kpage = palloc_get_page (PAL_USER);
  struct frame *frame;

  if (kpage != NULL)
    {
      frame = malloc (sizeof (struct frame));
      if (!frame)
        {
          return NULL;
        }
      frame->kpage = kpage;
    }
  else
    {
      frame = frame_evict ();
    }

  frame->upage = upage;
  frame->t = thread_current ();
  lock_acquire (&ftable_lock);
  list_push_back (&ftable, &frame->elem);
  lock_release (&ftable_lock);

  return frame->kpage;
}

/* Evict a frame.

   Uses the second-chance algorithm to evict a frame from
   the frame table.

   NB: This function is synchronized with page faults and
       with the destruction of a process' supplementary page table.
   Returns the address of the evicted frame, or NULL on error.
*/
void *
frame_evict (void)
{
  struct frame *frame;
  struct list_elem *e;
  struct supp_pte *pte;
  bool write_to_disk = false;
  bool write_to_swap = false;
  int swap_slot_index;
  bool found_frame = false;

  /* Evict the first frame that is:
       a) unaccessed, and
       b) not pinned.
     Clears the accessed bit of each frame that is passess,
     so, in the worst case (barring pinning),
     every frame will be examined once and one will be examined twice. */
  lock_acquire (&ftable_lock);
  while (!found_frame)
    {
      e = list_front (&ftable);
      frame = list_entry(e, struct frame, elem);

      /* Acquire the lock to ensure that the lookup does not clash
         with an insertion into or removal from the supplementary page table. */
      lock_acquire (&frame->t->supp_pt.lock);
      pte = supp_pt_lookup (&frame->t->supp_pt, frame->upage);
      lock_release (&frame->t->supp_pt.lock);

      lock_acquire (&pte->l);
      if (pte->pinned)
        {
          lock_release (&pte->l);
          list_push_back(&ftable, list_pop_front (&ftable));
        }
      else if (pagedir_is_accessed (frame->t->pagedir, frame->upage))
        {
          pagedir_set_accessed (frame->t->pagedir, frame->upage, false);
          lock_release (&pte->l);
          list_push_back(&ftable, list_pop_front (&ftable));
        }
      else
        {
          lock_acquire (&frame->t->supp_pt.lock);
          frame->t->supp_pt.num_updating++;
          lock_release (&frame->t->supp_pt.lock);

          /* Mark this supplementary page table entry as being evicted
             in order to avoid a race with another thread attempting to
             fault the page back in. */
          pte->being_evicted = true;

          /* Use the filesystem as the backing store for read-only data
             and for mapped data; use swap as a backing store for all other
             data. */
          if ((pte->file != NULL && !pte->writable))
            {
              pte->loc = DISK;
            }
          else if (supp_pt_is_valid_mapping (pte->mapping))
            {
              pte->loc = DISK;
              if (pagedir_is_dirty (frame->t->pagedir, frame->upage))
                {
                  write_to_disk = true;
                }
            }
          else
            {
              pte->loc = SWAP;
              write_to_swap = true;
            }

          /* Clear the page directory entry for the evicted
             upage and pop the frame. */
          pagedir_clear_page (frame->t->pagedir,
            frame->upage);
          lock_release (&pte->l);
          list_pop_front (&ftable);
          found_frame = true;
        }
    }
  lock_release (&ftable_lock);

  /* Write data to its backing store.

     NB: This is done without holding the ftable_lock
         to ensure that I/O does not block other page faults. */
  if (write_to_swap)
    {
      swap_slot_index = swap_write_page (frame->kpage);
    }
  else if (write_to_disk)
    {
      supp_pt_write_if_mapping (pte);
    }

  /* If a thread page faulted on the evicted page and was
     waiting for eviction to finish, iform it that eviction
     has indeed finished. */
  lock_acquire (&pte->l);
  if (write_to_swap)
    {
      pte->swap_slot_index = swap_slot_index;
    }
  pte->being_evicted = false;
  cond_signal (&pte->done_evicting, &pte->l);
  lock_release (&pte->l);

  /* If no threads are currently evicting any entry belonging to
     the evicted thread's supp_pt, then inform the thread that it
     can destory its supp_pt if it so desires. */
  lock_acquire (&frame->t->supp_pt.lock);
  frame->t->supp_pt.num_updating--;
  if (frame->t->supp_pt.num_updating == 0)
    {
      cond_signal (&frame->t->supp_pt.done_updating, &frame->t->supp_pt.lock);
    }
  lock_release (&frame->t->supp_pt.lock);

  memset (frame->kpage, 0, PGSIZE);
#ifndef NDEBUG
  memset (frame->kpage, 0xcc, PGSIZE);
#endif
  return frame;
}

/* Free the resources owned by the supplied frame; if the frame's
   upage was memory mapped, write it to disk if said upage is dirty. */
static void
frame_unalloc (struct frame *f)
{
  ASSERT (is_user_vaddr (f->upage));
  ASSERT (pagedir_get_page (f->t->pagedir, f->upage) == f->kpage);

  if (pagedir_is_dirty (f->t->pagedir, f->upage))
    {
      supp_pt_write_if_mapping (supp_pt_lookup (&f->t->supp_pt, f->upage));
    }
  pagedir_clear_page (f->t->pagedir, f->upage);
  palloc_free_page (f->kpage);
  free (f);
}

/* Free the frame corresponding to kpage and any resources it may hold,
   and remove it from the frame table.

   NB: If no corresponding frame is found, then this function
   has no side effects. */
void
frame_free (void *kpage)
{
  struct list_elem *e;
  lock_acquire (&ftable_lock);
  for (e = list_begin (&ftable); e != list_end (&ftable);
       e = list_next (e))
    {
      struct frame *f = list_entry (e, struct frame, elem);
      if (f->kpage == kpage)
        {
          list_remove (&f->elem);
          frame_unalloc (f);
          break;
        }
    }
  lock_release (&ftable_lock);
}

/* Prune the frame table of all frames allocated
   for the given thread and unallocate each frame.

   This function guarantees that I/O will not block page
   faults or eviction. */
void
frame_free_thread (struct thread *t)
{
  struct frame *f;
  struct list retired_frames;
  list_init (&retired_frames);

  /* We first prune the ftable of all frames corresponding to
     the supplied thread, remembering them in an auxiliary list. */
  lock_acquire (&ftable_lock);
  struct list_elem *e = list_begin (&ftable);
  while (e != list_end (&ftable))
    {
      f = list_entry (e, struct frame, elem);
      if (f->t == t)
        {
          e = list_remove (&f->elem);
          list_push_back (&retired_frames, &f->elem);
        }
      else
        {
          e = list_next (e);
        }
    }
  lock_release (&ftable_lock);

  /* The frame table lock has been released, so we can now
     free each frame and perform writes if necessary. */
  e = list_begin (&retired_frames);
  while (!list_empty (&retired_frames))
    {
      f = list_entry (e, struct frame, elem);
      e = list_remove (&f->elem);
      frame_unalloc (f);
    }
}
