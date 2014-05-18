#include "lib/stdint.h"
#include "lib/debug.h"
#include "lib/kernel/list.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/frame.h"

static struct list ftable;

struct frame
  {
    void *kpage;
    void *upage;
    struct list_elem elem; // list_elem for frame table
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

  ASSERT (kpage != NULL); // TODO: implement swapping

  // now record this in our frame table
  struct frame *frame = malloc (sizeof (struct frame));
  if (!frame)
    {
      return NULL; // error!
    }
  frame->kpage = kpage;
  frame->upage = upage;
  /* TODO: Process identifier. */
  list_push_back (&ftable, &frame->elem); // add frame to our frame table

  return kpage;
}

/* Free the page kpage and remove the corresponding entry
   from our frame table. */
void
frame_free (void *kpage)
{
  struct list_elem *e;
  for (e = list_begin (&ftable); e != list_end (&ftable);
       e = list_next (e))
    {
      struct frame *f = list_entry (e, struct frame, elem);
      if (f->kpage == kpage)
        {
          list_remove (&f->elem);
          palloc_free_page (kpage);
          break;
        }
    }
}
