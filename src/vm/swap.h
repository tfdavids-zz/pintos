#ifndef VM_SWAP_H
#define VM_SWAP_H

#include "lib/stdbool.h"
#include "lib/stddef.h"

/* Initilization. */
void swap_init (void);

/* I/O. */
size_t swap_write_page (void *kpage);
bool swap_load_page (size_t slot_index, void *kpage);

/* Removal. */
bool swap_free (size_t slot_index);

#endif /* VM_SWAP_H */


