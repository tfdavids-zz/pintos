#ifndef VM_FRAME_H
#define VM_FRAME_H

// grab a new frame and return the address
void *frame_alloc (void);
void frame_free (void *page);

void frame_init (void);

#endif
