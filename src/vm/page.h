#ifndef VM_PAGE_H
#define VM_PAGE_H

/* On a page fault, the kernel looks up the virtual page that faulted in the
 * supplemental page table to find out what data should be there. This means
 * that each entry in this table needs to point to the data that the user
 * thinks is at virtual address `address`, and include any necessary metadata
 * about that data.
 */

// where is the data?
enum data_loc {
	DISK,
	ZEROES,
	SWAP
};

struct supp_pte {
	void *address;
	bool writable;

	enum data_loc loc;

	// if the page should be in memory, we need this
	struct file *file;
	off_t start;

	struct hash_elem *hash_elem;
};

// look up supp_pte based on given address
struct supp_pte *supp_pte_lookup (void *address);

// insert a supplemental page table entry for user memory at `address`
void supp_pte_insert (void *address);

#endif /* VM_PAGE_H */
