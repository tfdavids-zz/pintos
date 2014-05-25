#ifndef VM_FRAME_H
#define VM_FRAME_H

#include "threads/thread.h"

/* Initialization. */
void frame_table_init (void);

/* Frame creation. */
void *frame_alloc (void *upage);

/* Frame disposal. */
void frame_free (void *page);
void frame_free_thread (struct thread *t);

/* Frame eviction. */
void *frame_evict (void);

#endif /* VM_FRAME_H */
