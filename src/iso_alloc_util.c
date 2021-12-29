/* iso_alloc_util.c - A secure memory allocator
 * Copyright 2021 - chris.rohlf@gmail.com */

#include "iso_alloc_internal.h"

INTERNAL_HIDDEN void *create_guard_page(void *p) {
    if(p == NULL) {
        p = mmap_rw_pages(g_page_size, false, GUARD_PAGE_NAME);

        if(p == NULL) {
            LOG_AND_ABORT("Could not allocate guard page");
        }
    }

    /* Use g_page_size here because we could be
     * calling this while we setup the root */
    mprotect_pages(p, g_page_size, PROT_NONE);
    madvise(p, g_page_size, MADV_DONTNEED);
    return p;
}

INTERNAL_HIDDEN void *mmap_rw_pages(size_t size, bool populate, const char *name) {
    return mmap_pages(size, populate, name, PROT_READ | PROT_WRITE);
}

INTERNAL_HIDDEN void *mmap_pages(size_t size, bool populate, const char *name, int32_t prot) {
#if !ENABLE_ASAN
    /* Produce a random page address as a hint for mmap */
    uint64_t hint = ROUND_DOWN_PAGE(rand_uint64());
    hint &= 0x3FFFFFFFF000;
    void *p = (void *) hint;
#else
    void *p = NULL;
#endif

    size = ROUND_UP_PAGE(size);

    /* Only Linux supports MAP_POPULATE */
#if __linux__ && PRE_POPULATE_PAGES
    if(populate == true) {
        p = mmap(p, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    } else {
        p = mmap(p, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
#else
    p = mmap(p, size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif

    if(p == MAP_FAILED) {
        LOG_AND_ABORT("Failed to mmap rw pages");
        return NULL;
    }

    if(name != NULL) {
        name_mapping(p, size, name);
    }

    return p;
}

INTERNAL_HIDDEN void mprotect_pages(void *p, size_t size, int32_t protection) {
    size = ROUND_UP_PAGE(size);

    if((mprotect(p, size, protection)) == ERR) {
        LOG_AND_ABORT("Failed to mprotect pages @ 0x%p", p);
    }
}

INTERNAL_HIDDEN int32_t name_zone(iso_alloc_zone *zone, char *name) {
#if NAMED_MAPPINGS && __ANDROID__
    return name_mapping(zone->user_pages_start, ZONE_USER_SIZE, (const char *) name);
#else
    return 0;
#endif
}

INTERNAL_HIDDEN int32_t name_mapping(void *p, size_t sz, const char *name) {
#if NAMED_MAPPINGS && __ANDROID__
    return prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, p, sz, name);
#else
    return 0;
#endif
}

INTERNAL_HIDDEN INLINE size_t next_pow2(size_t sz) {
    sz |= sz >> 1;
    sz |= sz >> 2;
    sz |= sz >> 4;
    sz |= sz >> 8;
    sz |= sz >> 16;
    sz |= sz >> 32;
    return sz + 1;
}
