#ifndef VM_PAGE_H
#define VM_PAGE_H

/* On a page fault, the kernel looks up the virtual page that faulted in the
 * supplemental page table to find out what data should be there. This means
 * that each entry in this table needs to point to the data that the user
 * thinks is at virtual address `address`, and include any necessary metadata
 * about that data.
 */


bool page_table_init (struct hash *h);

// initialize (in the supplemental page table) a virtual page at (virtual) address upage
bool page_alloc (struct hash *h, void *upage, bool writable);

// handle a page fault (obtain a frame, fetch the right data into the frame, point the VA to the frame, and return success)
bool page_handle_fault (struct hash *h, void *upage);

void page_free (struct hash *h, void *upage);

#endif /* VM_PAGE_H */
