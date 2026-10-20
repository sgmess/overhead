#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_INTERNAL 0
#define heap_caps_calloc(n, s, c) calloc(n, s)
#define heap_caps_malloc(s, c) malloc(s)
#define heap_caps_realloc(p, s, c) realloc(p, s)
#define heap_caps_free free
static inline size_t heap_caps_get_free_size(int c)
{
    (void)c;
    return 100000;
}
