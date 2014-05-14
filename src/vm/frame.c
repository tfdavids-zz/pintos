#include "lib/stdint.h"
#include "lib/debug.h"
#include "lib/kernel/list.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/frame.h"

static struct list *ftable;

struct frame
  {
    void *pg_addr;
    struct list_elem elem; // list_elem for frame table
  };

void
frame_init (void)
{
  list_init (&ftable);
}

void *
frame_alloc (void)
{
  void *page = palloc_get_page (PAL_USER);
  
  ASSERT (page != NULL); // TODO: implement swapping

  // now record this in our frame table
  struct frame *frame = malloc (sizeof (struct frame));
  frame->pg_addr = page;
  list_push_back (ftable, &frame->elem); // add frame to our frame table

  return page;
}

void
frame_free (void *page) {
  palloc_free_page (page);
  // TODO
}
