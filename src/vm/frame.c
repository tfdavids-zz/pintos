#include "lib/stdint.h"
#include "lib/debug.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/frame.h"

struct frame
  {
    void *pg_addr;
    struct list_elem elem; // list_elem for frame table
  };

struct list *ftable;
// somewhere: list_init (ftable);

void *
alloc_frame (void)
{
  void *page = palloc_get_page (PAL_USER);
  
  ASSERT (page != NULL); // TODO: implement swapping

  struct frame *frame = malloc (sizeof (struct frame));
  frame->pg_addr = page;
  list_push_back (ftable, frame); // add fte to our frame table

  return page;
}
