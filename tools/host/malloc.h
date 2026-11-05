/* malloc.h for host tests on macOS, which keeps these in <malloc/malloc.h>. */
#include <stdlib.h>
#include <malloc/malloc.h>
#define malloc_usable_size(p) malloc_size(p)
