#ifndef VM_FRAME_H
#define VM_FRAME_H

// grab a new frame and return the address
void *frame_alloc (void);

// free the frame pointed at by page
void frame_free (void *page);

// initialize the frame table
void frame_table_init (void);

#endif
