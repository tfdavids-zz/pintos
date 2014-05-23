#include "vm/frame.h"

#include <stdio.h>
#include <string.h>

#include "lib/stdint.h"
#include "lib/debug.h"
#include "lib/kernel/list.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"

static struct list ftable;
static struct lock ftable_lock;

struct frame
  {
    void *kpage;
    void *upage;
    struct list_elem elem; // list_elem for frame table
    struct thread *t;
    bool pinned;
    // TODO: identifier for process?
  };

void
frame_table_init (void)
{
  list_init (&ftable);
  lock_init (&ftable_lock);
}

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
          /* TODO:Kernel pool full, should probably panic */
          return NULL; // error!
        }
      frame->kpage = kpage;
    }
  else
    {
      frame = frame_evict ();
    }

  frame->upage = upage;
  frame->t = thread_current ();
  frame->pinned = false;
  /* TODO: Process identifier. */
  lock_acquire (&ftable_lock);
  list_push_back (&ftable, &frame->elem); // add frame to our frame table
  lock_release (&ftable_lock);

  return frame->kpage;
}

void *
frame_evict (void)
{
  struct frame *frame;
  struct frame *evicted_frame = NULL;
  struct list_elem *e;
  struct supp_pte *pte;

  /* Find an old(unaccessed) frame to evict */
  lock_acquire (&ftable_lock);
  while (evicted_frame == NULL)
    {
      e = list_front (&ftable);
      frame = list_entry(e, struct frame, elem);
      pte = supp_pt_lookup (&frame->t->supp_pt, frame->upage);
      if (pte->pinned)
        {
          list_push_back(&ftable, list_pop_front (&ftable));
        }
      else if (pagedir_is_accessed (frame->t->pagedir, frame->upage))
        {
          pagedir_set_accessed (frame->t->pagedir, frame->upage, false);
          list_push_back(&ftable, list_pop_front (&ftable));
        }
      else
        {
          evicted_frame = frame;
          list_pop_front (&ftable);

          /* TODO: Get this synchronization right. */
          struct thread *t = evicted_frame->t;

          lock_acquire (&t->supp_pt.lock);
          pte = supp_pt_lookup (
            &t->supp_pt, evicted_frame->upage);

          /* Use the filesystem as a backing store for RO data and for mmap-ed
             files, when appropriate. */
          if ((pte->file != NULL && !pte->writable) ||
              (supp_pt_is_valid_mapping (pte->mapping)))
            {
              pte->loc = DISK;
              /* TODO: IO shouldn't block! */
              supp_pt_write_if_dirty (pte);
            }
          else
            {
              pte->loc = SWAP;

              /* TODO: It is wasteful to do IO here. */
              pte->swap_slot_index = swap_write_page (evicted_frame->kpage);
            }
          pagedir_clear_page (t->pagedir, evicted_frame->upage);
          lock_release (&t->supp_pt.lock);
        }
    }
  lock_release (&ftable_lock);


  /* Swap out the page if necessary.*/
  /* TODO: Get this right. */


  /* TODO should it be cc or 0 ? */
  memset (evicted_frame->kpage, 0, PGSIZE);

#ifndef NDEBUG
  memset (evicted_frame->kpage, 0xcc, PGSIZE);
#endif
  return evicted_frame;
}

static void
frame_unalloc (struct frame *f)
{
  supp_pt_write_if_dirty (supp_pt_lookup (&f->t->supp_pt, f->upage));

  palloc_free_page (f->kpage);
  pagedir_clear_page (f->t->pagedir, f->upage);
  free (f);
}

/* Free the page kpage and remove the corresponding entry
   from our frame table.
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

/* Prunes the frame table of all frames allocated
   for the given thread. */
void
frame_free_thread (struct thread *t)
{
  lock_acquire (&ftable_lock);
  struct list_elem *e = list_begin (&ftable);
  while (e != list_end (&ftable))
    {
      struct frame *f = list_entry (e, struct frame, elem);
      if (f->t == t)
        {
          e = list_remove (&f->elem);
          frame_unalloc (f);
        }
      else
        {
          e = list_next (e);
        }
    }
  lock_release (&ftable_lock);
}
