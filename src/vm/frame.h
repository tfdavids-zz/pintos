#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

// grab a new frame and return the address
void *frame_alloc (void *upage);

// free the frame pointed at by page
void frame_free (void *page);

void frame_free_thread (struct thread *t);

// initialize the frame table
void frame_table_init (void);

// evict frame
void *frame_evict (void);

#endif /* VM_FRAME_H */
