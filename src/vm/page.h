#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/stdbool.h"
#include "lib/kernel/hash.h"
#include "devices/block.h"
#include "filesys/off_t.h"
#include "filesys/file.h"

/* On a page fault, the kernel looks up the virtual page that faulted in the
 * supplemental page table to find out what data should be there. This means
 * that each entry in this table needs to point to the data that the user
 * thinks is at virtual address `address`, and include any necessary metadata
 * about that data.
 */

enum data_loc
  {
    DISK,
    ZEROES,
    SWAP
  };

bool supp_pt_init (struct hash *h);
void supp_pt_destroy (struct hash *h);

// initialize (in the supplemental page table) a virtual page at (virtual)
// address upage, backed by data from the given sector on the block
bool page_alloc (struct hash *h, void *upage, enum data_loc loc,
			     struct block *block, block_sector_t sector,
			     struct file *file, off_t start,
			     bool writable);

// handle a page fault (obtain a frame, fetch the right data into the frame,
// point the VA to the frame, and return success)
bool page_handle_fault (struct hash *h, void *upage);

// check and see if a page has an entry in h
bool page_is_valid (struct hash *h, void *upage);

// free a virtual page with address upage
/* TODO: Do we need this? Also, a conceptual question -- when a user invokes
  'free' on some memory he has, should we / how would we go about updating the
  supplemental page table? */
void page_free (struct hash *h, void *upage);

#endif /* VM_PAGE_H */
