#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/stdbool.h"
#include "lib/kernel/hash.h"
#include "userprog/syscall.h"
#include "devices/block.h"
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "threads/synch.h"

/* Locations that a given page may reside in;
   ZEROES corresponds to unitialized data. */
enum data_loc
  {
    MEMORY,
    DISK,
    SWAP,
    ZEROES,
  };

/* The supplementary page table structure.

   A note on synchronization: The table acquires the
   lock upon insertions and deletions, but does not acquire
   the lock at any other time. The user must take care to
   synchronize access to the table. */
struct supp_pt
  {
    struct hash h;                  /* The supp_t is backed by a hash table. */
    struct lock lock;               /* For synchronizing access to the table. */
    size_t num_updating;            /* The number of threads currently updating
                                       entries within the table. */
    struct condition done_updating; /* The table can only be destroyed when
                                       no threads are updating its entries. */
  };

/* Each entry within the supplementary page table is a struct supp_pte,
   or supplementary page table entry. Entries are keyed by the user page.

   The supplementary page table contains an entry for a given user virtual
   address (page) if and only if the user owns that page.  (Note that a
   mapping for the page need not necessarily be present in the page directory,
   sinces pages are lazy-loaded and can be paged out.)
   */
struct supp_pte
  {
    void *upage;            /* The user virtual address, rounded down to
                               the nearest page. */
    bool writable;          /* True if and only if the user has write access to
                               the page. */
    enum data_loc loc;      /* Where the page resides in our system. */
    size_t swap_slot_index; /* If loc == SWAP, the index into the swap table
                               that corresponds to where this upage's data is
                               stored on swap. */
    struct file *file;      /* The file from which the data was loaded, if any.
                             */
    off_t start;            /* The offset in the file at which the data begin.
                             */
    size_t bytes;           /* The number of bytes in the file spanned by the
                               data. */
    mapid_t mapping;        /* Negative if this page was not mmaped; otherwise,
                               the ID of the mapping. IDs are unique per
                               per process. */
    bool pinned;            /* True if the frame corresponding to
                               this entry should not be evicted. */
    bool being_evicted;     /* True if and only if the corresponding frame is
                               being evicted. */
    struct condition done_evicting; /* To syncrhonize with eviction. */
    struct lock l;                  /* To synchronize with eviction. */
    struct hash_elem hash_elem; /* To hook the entry into the supp_pte. */
  };

/* Initialization. */
bool supp_pt_init (struct supp_pt *supp_pt);

/* Allocation. */
bool supp_pt_page_alloc_file (struct supp_pt *supp_pt, void *upage,
                            struct file *file, off_t start, size_t bytes,
                            mapid_t mapid, bool writable);
bool supp_pt_page_calloc (struct supp_pt *supp_pt, void *upage, bool writable);

/* Page faults and loading. */
bool supp_pt_handle_fault (struct supp_pt *supp_pt, void *upage);
bool supp_pt_force_load (struct supp_pte *e);
bool supp_pt_grow_stack_if_necessary (struct supp_pt *supp_pt,
  void *esp, void *addr);

/* Auxiliary functions. */
struct supp_pte *supp_pt_lookup (struct supp_pt *supp_pt, const void *addr);
bool supp_pt_is_valid_mapping (mapid_t mapping);
void supp_pt_write_if_mapping (struct supp_pte *supp_pte);

/* Disposal. */
void supp_pt_page_free (struct supp_pt *supp_pt, void *upage);
bool supp_pt_munmap (struct supp_pt *supp_pt, void *first_mmap_page);
void supp_pt_destroy (struct supp_pt *supp_pt);

#endif /* VM_PAGE_H */
