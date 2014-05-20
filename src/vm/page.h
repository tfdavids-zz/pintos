#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/stdbool.h"
#include "lib/kernel/hash.h"
#include "lib/user/syscall.h"
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
    SWAP,
    PRESENT,
  };

struct supp_pte
  {
    const void *address;
    bool writable;

    enum data_loc loc;

    // for pages on swap, we need this
    size_t swap_slot_index;

    // and for pages on disk, we need this
    struct file *file; /* The file backing this page. */
    off_t start;  /* The offset in the file at which the data begin. */
    size_t bytes; /* The number of bytes to copy from the file. */
    mapid_t mapping; /* Negative if this page was not mmaped; otherwise,
                        the id of the mapping. */

    struct hash_elem hash_elem;
  };

bool supp_pt_init (struct hash *h);
void supp_pt_destroy (struct hash *h);

/* Hook the provided upage into a user process' supplementary page
   table. */
bool supp_pt_page_alloc_file (struct hash *h, void *upage, struct file *file,
                            off_t start, size_t bytes,
                            mapid_t mapid, bool writable);
bool supp_pt_page_calloc (struct hash *h, void *upage, bool writable);

/* Creates a mapping between the provided upage and to an allocated
   kpage; returns the kpage. */
void *page_force_load (struct hash *h, void *upage);

// handle a page fault (obtain a frame, fetch the right data into the frame,
// point the VA to the frame, and return success)
bool page_handle_fault (struct hash *h, void *upage);

// check and see if a page has an entry in h
bool supp_pt_page_exists (struct hash *h, const void *upage);

// free a virtual page with address upage
/* TODO: Do we need this? Also, a conceptual question -- when a user invokes
  'free' on some memory he has, should we / how would we go about updating the
  supplemental page table? */
bool supp_pt_page_free (struct hash *h, void *upage);
bool supp_pt_munmap (struct hash *h, void *first_mmap_page);

struct supp_pte *supp_pt_lookup (struct hash *h, void *address);

#endif /* VM_PAGE_H */
