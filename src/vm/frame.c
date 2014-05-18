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
#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "userprog/pagedir.h"

static struct list ftable;

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
}

void *
frame_alloc (void *upage)
{
  void *kpage = palloc_get_page (PAL_USER);

  if (kpage == NULL)
    {
      kpage = frame_evict ();
    }

  // now record this in our frame table
  struct frame *frame = malloc (sizeof (struct frame));

  if (!frame)
    {
      /* TODO:Kernel pool full, should probably panic */
      return NULL; // error!
    }
  frame->kpage = kpage;
  frame->upage = upage;
  frame->t = thread_current ();
  frame->pinned = false;
  /* TODO: Process identifier. */
  list_push_back (&ftable, &frame->elem); // add frame to our frame table

  return kpage;
}

void *
frame_evict (void)
{
  struct frame *frame;
  struct frame *evicted_frame = NULL;
  struct list_elem *e;
  struct supp_pte *pte;

  /* Find an old(unaccessed) frame to evict */
  while (evicted_frame == NULL)
    {
      for (e = list_begin (&ftable); e != list_end (&ftable);
           e = list_next (e))
        {
          frame = list_entry(e, struct frame, elem);
          if (pagedir_is_accessed (frame->t->pagedir, frame->upage))
            {              
              pagedir_set_accessed (frame->t->pagedir, frame->upage, false);
            }
          else
            {
              evicted_frame = frame;
              break;
            }
        }
    }
  
  pte = supp_pte_lookup (&evicted_frame->t->supp_pt, evicted_frame->upage);

  /* Swap out the page */
  pte->swap_slot_index = swap_write_page (evicted_frame->upage);

  pte->loc = SWAP;
  
  /* TODO should it be cc or 0 ? */
  memset (frame->kpage, 0, PGSIZE);

#ifndef NDEBUG
  memset (frame->kpage, 0xcc, PGSIZE);
#endif
  pagedir_clear_page (evicted_frame->t->pagedir, frame->upage);

  return frame->kpage;
}


void
frame_free (void *kpage)
{
  palloc_free_page (kpage);
  // TODO
}
