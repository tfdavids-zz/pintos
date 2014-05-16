#ifndef VM_SWAP_H
#define VM_SWAP_H

void swap_init (void);

size_t swap_writes_page (void *upage);

void swap_load_page (size_t swap_index, void *upage);

#endif /* VM_SWAP_H */


