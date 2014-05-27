#ifndef CACHE_H
#define CACHE_H

#include "devices/block.h"

void cache_init (void);
void cache_add (struct block *block, block_sector_t sector);
void *cache_get (struct block *block, block_sector_t sector);

#endif /* CACHE_H */
