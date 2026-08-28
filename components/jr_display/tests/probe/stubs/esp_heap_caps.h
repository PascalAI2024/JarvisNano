#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM (1<<0)
#define MALLOC_CAP_8BIT (1<<1)
#define MALLOC_CAP_INTERNAL (1<<2)
#define MALLOC_CAP_DMA (1<<3)
void *heap_caps_malloc(size_t s, unsigned caps);
void *heap_caps_calloc(size_t n, size_t s, unsigned caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(unsigned caps);
