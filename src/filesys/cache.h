#ifndef CACHE_H
#define CACHE_H

#include "devices/block.h"

void cache_init (void);
void cache_read (struct block *block, block_sector_t sector, void *buffer);
void cache_write (struct block *block, block_sector_t sector, const void *buffer);
void cache_flush (void);

#endif /* CACHE_H */
