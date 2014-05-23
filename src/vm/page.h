#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/stdbool.h"
#include "lib/kernel/hash.h"
#include "userprog/syscall.h"
#include "devices/block.h"
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "threads/synch.h"

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
  };

struct supp_pt
  {
    struct hash h;
    struct lock lock;
  };

struct supp_pte
  {
    void *upage;
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
    bool pinned;
    struct hash_elem hash_elem;
  };

bool supp_pt_init (struct supp_pt *supp_pt);
void supp_pt_destroy (struct supp_pt *supp_pt);

/* Hook the provided upage into a user process' supplementary page
   table. */
bool supp_pt_page_alloc_file (struct supp_pt *supp_pt, void *upage,
                            struct file *file, off_t start, size_t bytes,
                            mapid_t mapid, bool writable);
bool supp_pt_page_calloc (struct supp_pt *supp_pt, void *upage, bool writable);

/* Writes the contents of the supplied mapping to the filesystem,
   if said contents are dirty. */
void supp_pt_write_if_dirty (struct supp_pte *supp_pte);

/* Creates a mapping between the provided upage and to an allocated
   kpage; returns the kpage. */
void *page_force_load (struct supp_pt *supp_pt, void *upage);

// handle a page fault (obtain a frame, fetch the right data into the frame,
// point the VA to the frame, and return success)
bool page_handle_fault (struct supp_pt *supp_pt, void *upage);

// check and see if a page has an entry in h
bool supp_pt_page_exists (struct supp_pt *supp_pt, void *upage);

void supp_pt_page_free (struct supp_pt *supp_pt, void *upage);

// free a virtual page with address upage
bool supp_pt_munmap (struct supp_pt *supp_pt, void *first_mmap_page);
bool supp_pt_is_valid_mapping (mapid_t mapping);

struct supp_pte *supp_pt_lookup (struct supp_pt *supp_pt, void *upage);
#endif /* VM_PAGE_H */
