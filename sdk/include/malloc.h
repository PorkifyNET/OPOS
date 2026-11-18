/* OPOS SDK C library: malloc extras. */
#ifndef OPOS_MALLOC_H
#define OPOS_MALLOC_H

#include <stdlib.h>

/* Usable bytes of a block from malloc (at least the size asked for). */
size_t malloc_usable_size(void *memory);

#endif
