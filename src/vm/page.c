struct hash *h; // supplemental page table
// somewhere: hash_init (h, ...)

void supp_pte_insert (void *address) {
	struct supp_pte *e = malloc (sizeof (struct supp_pte));

	e->address = address;
	hash_init (e->hash_elem);

	hash_insert (h, e->hash_elem);
}

struct supp_pte *supp_pte_lookup (void *address) {
	address = pg_round_down (address); // round down to page
	
	struct supp_pte *key;
	key->address = address;
	hash_elem *e = hash_find (h, key); // find the element in our hash table corresponding to this page

	struct supp_pte *pte = hash_entry (e, struct supp_pte, hash_elem);
	return pte;
}
