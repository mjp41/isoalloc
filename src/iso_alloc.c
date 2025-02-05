/* iso_alloc.c - A secure memory allocator
 * Copyright 2022 - chris.rohlf@gmail.com */

#include "iso_alloc_internal.h"

#if THREAD_SUPPORT

#if USE_SPINLOCK
atomic_flag root_busy_flag;
atomic_flag big_zone_busy_flag;
#else
pthread_mutex_t root_busy_mutex;
pthread_mutex_t big_zone_busy_mutex;
#endif

/* We cannot initialize these on thread creation so
 * we can't mmap them somewhere with guard pages but
 * they are thread local storage so their location
 * won't be as predictable as .bss */
static __thread _tzc zone_cache[ZONE_CACHE_SZ];
static __thread size_t zone_cache_count;

static __thread uintptr_t chunk_quarantine[CHUNK_QUARANTINE_SZ];
static __thread size_t chunk_quarantine_count;
#else
/* When not using thread local storage we can mmap
 * these pages somewhere safer than global memory
 * and surrounded by guard pages */
static _tzc *zone_cache;
static size_t zone_cache_count;

static uintptr_t *chunk_quarantine;
static size_t chunk_quarantine_count;
#endif

uint32_t g_page_size;
iso_alloc_root *_root;

/* Zones are linked by their next_sz_index member which
 * tells the allocator where in the _root->zones array
 * it can find the next zone that holds the same size
 * chunks. The lookup table helps us find the first zone
 * that holds a specific size in O(1) time */
static zone_lookup_table_t *zone_lookup_table;

/* The chunk to zone lookup table provides a high hit
 * rate cache for finding which zone owns a user chunk.
 * It works by mapping the MSB of the chunk addressq
 * to a zone index. Misses are gracefully handled and
 * more common with a higher RSS and more mappings. */
static chunk_lookup_table_t *chunk_lookup_table;

#if NO_ZERO_ALLOCATIONS
void *_zero_alloc_page;
#endif

/* Select a random number of chunks to be canaries. These
 * can be verified anytime by calling check_canary()
 * or check_canary_no_abort() */
INTERNAL_HIDDEN void create_canary_chunks(iso_alloc_zone_t *zone) {
#if ENABLE_ASAN || DISABLE_CANARY
    return;
#else
    /* Canary chunks are only for default zone sizes. This
     * is because larger zones would waste a lot of memory
     * if we set aside some of their chunks as canaries */
    if(zone->chunk_size > MAX_DEFAULT_ZONE_SZ) {
        return;
    }

    bitmap_index_t *bm = (bitmap_index_t *) zone->bitmap_start;
    bit_slot_t bit_slot;

    const bitmap_index_t max_bitmap_idx = GET_MAX_BITMASK_INDEX(zone) - 1;
    const uint64_t chunk_count = GET_CHUNK_COUNT(zone);

    /* Roughly %1 of the chunks in this zone will become a canary */
    const uint64_t canary_count = (chunk_count / CANARY_COUNT_DIV);

    /* This function is only ever called during zone
     * initialization so we don't need to check the
     * current state of any chunks, they're all free.
     * It's possible the call to rand_uint64() here will
     * return the same index twice, we can live with
     * that collision as canary chunks only provide a
     * small security property anyway */
    for(uint64_t i = 0; i < canary_count; i++) {
        bitmap_index_t bm_idx = ALIGN_SZ_DOWN((rand_uint64() % (max_bitmap_idx)));

        if(0 > bm_idx) {
            bm_idx = 0;
        }

        /* We may have already chosen this index */
        if(GET_BIT(bm[bm_idx], 0)) {
            continue;
        }

        /* Set the 1st and 2nd bits as 1 */
        SET_BIT(bm[bm_idx], 0);
        SET_BIT(bm[bm_idx], 1);
        bit_slot = (bm_idx << BITS_PER_QWORD_SHIFT);
        void *p = POINTER_FROM_BITSLOT(zone, bit_slot);
        write_canary(zone, p);
    }
#endif
}

#if ENABLE_ASAN
INTERNAL_HIDDEN void verify_all_zones(void) {
    return;
}

INTERNAL_HIDDEN void verify_zone(iso_alloc_zone_t *zone) {
    return;
}

INTERNAL_HIDDEN void _verify_all_zones(void) {
    return;
}

INTERNAL_HIDDEN void _verify_zone(iso_alloc_zone_t *zone) {
    return;
}
#else

/* Verify the integrity of all canary chunks and the
 * canary written to all free chunks. This function
 * either aborts or returns nothing */
INTERNAL_HIDDEN void verify_all_zones(void) {
    LOCK_ROOT();
    _verify_all_zones();
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN void verify_zone(iso_alloc_zone_t *zone) {
    LOCK_ROOT();
    _verify_zone(zone);
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN void _verify_all_zones(void) {
    for(int32_t i = 0; i < _root->zones_used; i++) {
        iso_alloc_zone_t *zone = &_root->zones[i];

        if(zone->bitmap_start == NULL || zone->user_pages_start == NULL) {
            break;
        }

        _verify_zone(zone);
    }

    LOCK_BIG_ZONE();
    /* No need to lock big zone here since the
     * root should be locked by our caller */
    iso_alloc_big_zone_t *big = _root->big_zone_head;

    if(big != NULL) {
        big = UNMASK_BIG_ZONE_NEXT(_root->big_zone_head);
    }

    while(big != NULL) {
        check_big_canary(big);

        if(big->next != NULL) {
            big = UNMASK_BIG_ZONE_NEXT(big->next);
        } else {
            break;
        }
    }
    UNLOCK_BIG_ZONE();
}

INTERNAL_HIDDEN void _verify_zone(iso_alloc_zone_t *zone) {
    UNMASK_ZONE_PTRS(zone);
    const bitmap_index_t *bm = (bitmap_index_t *) zone->bitmap_start;
    const bitmap_index_t max_bm_idx = GET_MAX_BITMASK_INDEX(zone);
    bit_slot_t bit_slot;

    if(zone->next_sz_index > _root->zones_used) {
        LOG_AND_ABORT("Detected corruption in zone[%d] next_sz_index=%d", zone->index, zone->next_sz_index);
    }

    if(zone->next_sz_index != 0) {
        iso_alloc_zone_t *zt = &_root->zones[zone->next_sz_index];
        if(zone->chunk_size != zt->chunk_size) {
            LOG_AND_ABORT("Inconsistent chunk sizes for zones %d,%d with chunk sizes %d,%d", zone->index, zt->index, zone->chunk_size, zt->chunk_size);
        }
    }

    for(bitmap_index_t i = 0; i < max_bm_idx; i++) {
        for(int64_t j = 1; j < BITS_PER_QWORD; j += BITS_PER_CHUNK) {
            /* If this bit is set it is either a free chunk or
             * a canary chunk. Either way it should have a set
             * of canaries we can verify */
            if((GET_BIT(bm[i], j)) == 1) {
                bit_slot = (i << BITS_PER_QWORD_SHIFT) + j;
                const void *p = POINTER_FROM_BITSLOT(zone, bit_slot);
                check_canary(zone, p);
            }
        }
    }

    MASK_ZONE_PTRS(zone);
}
#endif

/* Pick a random index in the bitmap and start looking
 * for free bit slots we can add to the cache. The random
 * bitmap index is to protect against biasing the free
 * slot cache with only chunks towards the start of the
 * user mapping. Theres no guarantee this function will
 * find any free slots. */
INTERNAL_HIDDEN INLINE void fill_free_bit_slot_cache(iso_alloc_zone_t *zone) {
    const bitmap_index_t *bm = (bitmap_index_t *) zone->bitmap_start;
    const bitmap_index_t max_bitmap_idx = GET_MAX_BITMASK_INDEX(zone);

    /* This gives us an arbitrary spot in the bitmap to
     * start searching but may mean we end up with a smaller
     * cache. This may negatively affect performance but
     * leads to a less predictable free list */
    bitmap_index_t bm_idx;

    /* The largest max_bitmap_idx we will ever
     * have is 8192 for SMALLEST_CHUNK_SZ (16) */
    if(max_bitmap_idx > ALIGNMENT) {
        bm_idx = ((uint32_t) rand_uint64() * (max_bitmap_idx - 1) >> 32);
    } else {
        bm_idx = 0;
    }

    memset(zone->free_bit_slot_cache, BAD_BIT_SLOT, sizeof(zone->free_bit_slot_cache));
    zone->free_bit_slot_cache_usable = 0;
    uint8_t free_bit_slot_cache_index;

    for(free_bit_slot_cache_index = 0; free_bit_slot_cache_index < BIT_SLOT_CACHE_SZ; bm_idx++) {
        /* Don't index outside of the bitmap or
         * we will return inaccurate bit slots */
        if(UNLIKELY(bm_idx >= max_bitmap_idx)) {
            zone->free_bit_slot_cache_index = free_bit_slot_cache_index;
            return;
        }

        for(uint64_t j = 0; j < BITS_PER_QWORD; j += BITS_PER_CHUNK) {
            if(free_bit_slot_cache_index >= BIT_SLOT_CACHE_SZ) {
                zone->free_bit_slot_cache_index = free_bit_slot_cache_index;
                return;
            }

            if((GET_BIT(bm[bm_idx], j)) == 0) {
                zone->free_bit_slot_cache[free_bit_slot_cache_index] = (bm_idx << BITS_PER_QWORD_SHIFT) + j;
                free_bit_slot_cache_index++;
            }
        }
    }

#if SHUFFLE_BIT_SLOT_CACHE
    /* Shuffle the free bit slot cache */
    if(free_bit_slot_cache_index > 1) {
        for(uint8_t i = free_bit_slot_cache_index - 1; i > 0; i--) {
            uint8_t j = (uint8_t) (rand_uint64() % (i + 1));
            bit_slot_t t = zone->free_bit_slot_cache[j];
            zone->free_bit_slot_cache[j] = zone->free_bit_slot_cache[i];
            zone->free_bit_slot_cache[i] = t;
        }
    }
#endif

    zone->free_bit_slot_cache_index = free_bit_slot_cache_index;
}

INTERNAL_HIDDEN INLINE void insert_free_bit_slot(iso_alloc_zone_t *zone, int64_t bit_slot) {
#if VERIFY_BIT_SLOT_CACHE
    /* The cache is sorted at creation time but once we start
     * free'ing chunks we add bit_slots to it in an unpredictable
     * order. So we can't search the cache with something like
     * a binary search. This brute force search shouldn't incur
     * too much of a performance penalty as we only search starting
     * at the free_bit_slot_cache_usable index which is updated
     * everytime we call get_next_free_bit_slot(). We do this in
     * order to detect any corruption of the cache that attempts
     * to add duplicate bit_slots which would result in iso_alloc()
     * handing out in-use chunks. The _iso_alloc() path also does
     * a check on the bitmap itself before handing out any chunks */
    const int32_t max_cache_slots = (BIT_SLOT_CACHE_SZ >> 3);

    for(int32_t i = zone->free_bit_slot_cache_usable; i < max_cache_slots; i++) {
        if(zone->free_bit_slot_cache[i] == bit_slot) {
            LOG_AND_ABORT("Zone[%d] already contains bit slot %lu in cache", zone->index, bit_slot);
        }
    }
#endif

    if(zone->free_bit_slot_cache_index >= BIT_SLOT_CACHE_SZ) {
        return;
    }

    zone->free_bit_slot_cache[zone->free_bit_slot_cache_index] = bit_slot;
    zone->free_bit_slot_cache_index++;
}

INTERNAL_HIDDEN bit_slot_t get_next_free_bit_slot(iso_alloc_zone_t *zone) {
    if(zone->free_bit_slot_cache_usable >= BIT_SLOT_CACHE_SZ ||
       zone->free_bit_slot_cache_usable > zone->free_bit_slot_cache_index) {
        return BAD_BIT_SLOT;
    }

    zone->next_free_bit_slot = zone->free_bit_slot_cache[zone->free_bit_slot_cache_usable];
    zone->free_bit_slot_cache[zone->free_bit_slot_cache_usable++] = BAD_BIT_SLOT;
    return zone->next_free_bit_slot;
}

INTERNAL_HIDDEN INLINE void iso_clear_user_chunk(uint8_t *p, size_t size) {
    memset(p, POISON_BYTE, size);
}

INTERNAL_HIDDEN iso_alloc_root *iso_alloc_new_root(void) {
    void *p = NULL;
    iso_alloc_root *r;

    size_t _root_size = sizeof(iso_alloc_root) + (g_page_size << 1);

    p = (void *) mmap_rw_pages(_root_size, true, ROOT_NAME);

    if(p == NULL) {
        LOG_AND_ABORT("Cannot allocate pages for root");
    }

    r = (iso_alloc_root *) (p + g_page_size);
    r->system_page_size = g_page_size;
    r->guard_below = p;
    create_guard_page(r->guard_below);

    r->guard_above = (void *) ROUND_UP_PAGE((uintptr_t) (p + sizeof(iso_alloc_root) + r->system_page_size));
    create_guard_page(r->guard_above);
    return r;
}

INTERNAL_HIDDEN void iso_alloc_initialize_global_root(void) {
    /* Do not allow a reinitialization unless root is NULL */
    if(_root != NULL) {
        return;
    }

    _root = iso_alloc_new_root();

    if(_root == NULL) {
        LOG_AND_ABORT("Could not initialize global root");
    }

    /* We mlock the root or every allocation would
     * result in a soft page fault */
    MLOCK(&_root, sizeof(iso_alloc_root));

    _root->zones_size = (MAX_ZONES * sizeof(iso_alloc_zone_t));
    _root->zones_size += (g_page_size * 2);
    _root->zones_size = ROUND_UP_PAGE(_root->zones_size);

    /* Allocate memory with guard pages to hold zone data */
    void *p = mmap_rw_pages(_root->zones_size, false, NULL);
    create_guard_page(p);
    create_guard_page((void *) (uintptr_t) (p + _root->zones_size) - g_page_size);

    _root->zones = (void *) (p + g_page_size);
    name_mapping(p, _root->zones_size, "isoalloc zone metadata");

#if !THREAD_SUPPORT
    size_t c = ROUND_UP_PAGE(CHUNK_QUARANTINE_SZ * sizeof(uintptr_t));
    chunk_quarantine = mmap_rw_pages(c + (g_page_size * 2), true, NULL);
    create_guard_page(chunk_quarantine);
    chunk_quarantine = chunk_quarantine + (g_page_size / sizeof(uintptr_t));
    create_guard_page((void *) chunk_quarantine + c);
    MLOCK(chunk_quarantine, c);

    size_t z = ROUND_UP_PAGE(ZONE_CACHE_SZ * sizeof(_tzc));
    zone_cache = mmap_rw_pages(z + (g_page_size + 2), true, NULL);
    create_guard_page(zone_cache);
    zone_cache = ((void *) zone_cache) + g_page_size;
    create_guard_page((void *) zone_cache + z);
    MLOCK(zone_cache, z);
#endif

    /* If we don't lock the these lookup tables we may incur
     * a soft page fault with almost every alloc/free */
    zone_lookup_table = mmap_rw_pages(ZONE_LOOKUP_TABLE_SZ, true, NULL);
    MLOCK(&zone_lookup_table, ZONE_LOOKUP_TABLE_SZ);

    chunk_lookup_table = mmap_rw_pages(CHUNK_TO_ZONE_TABLE_SZ, true, NULL);
    MLOCK(&chunk_lookup_table, CHUNK_TO_ZONE_TABLE_SZ);

    for(int64_t i = 0; i < DEFAULT_ZONE_COUNT; i++) {
        if((_iso_new_zone(default_zones[i], true)) == NULL) {
            LOG_AND_ABORT("Failed to create a new zone");
        }
    }

    _root->zone_handle_mask = rand_uint64();
    _root->big_zone_next_mask = rand_uint64();
    _root->big_zone_canary_secret = rand_uint64();
}

__attribute__((constructor(FIRST_CTOR))) void iso_alloc_ctor(void) {
#if THREAD_SUPPORT && !USE_SPINLOCK
    pthread_mutex_init(&root_busy_mutex, NULL);
    pthread_mutex_init(&big_zone_busy_mutex, NULL);
#if ALLOC_SANITY
    pthread_mutex_init(&sane_cache_mutex, NULL);
#endif
#endif

    g_page_size = sysconf(_SC_PAGESIZE);
    iso_alloc_initialize_global_root();
#if HEAP_PROFILER
    _initialize_profiler();
#endif

#if NO_ZERO_ALLOCATIONS
    _zero_alloc_page = mmap_pages(g_page_size, false, NULL, PROT_NONE);
#endif

#if ALLOC_SANITY && UNINIT_READ_SANITY
    _iso_alloc_setup_userfaultfd();
#endif

#if ALLOC_SANITY
    _sanity_canary = rand_uint64();
#endif
}

INTERNAL_HIDDEN void flush_caches() {
    /* The thread zone cache can be invalidated
     * and does not require a lock */
    clear_zone_cache();

    LOCK_ROOT();
    _flush_chunk_quarantine();
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN INLINE void _flush_chunk_quarantine() {
    /* Free all the thread quarantined chunks */
    for(int64_t i = 0; i < chunk_quarantine_count; i++) {
        _iso_free_internal_unlocked((void *) chunk_quarantine[i], false, NULL);
    }

    clear_chunk_quarantine();
}

INTERNAL_HIDDEN void _unmap_zone(iso_alloc_zone_t *zone) {
    chunk_lookup_table[ADDR_TO_CHUNK_TABLE(zone->user_pages_start)] = 0;

    munmap(zone->bitmap_start, zone->bitmap_size);
    madvise(zone->bitmap_start, zone->bitmap_size, MADV_DONTNEED);
    munmap(zone->bitmap_start - _root->system_page_size, _root->system_page_size);
    madvise(zone->bitmap_start - _root->system_page_size, _root->system_page_size, MADV_DONTNEED);
    munmap(zone->bitmap_start + zone->bitmap_size, _root->system_page_size);
    madvise(zone->bitmap_start + zone->bitmap_size, _root->system_page_size, MADV_DONTNEED);

    munmap(zone->user_pages_start, ZONE_USER_SIZE);
    madvise(zone->user_pages_start, ZONE_USER_SIZE, MADV_DONTNEED);
    munmap(zone->user_pages_start - _root->system_page_size, _root->system_page_size);
    madvise(zone->user_pages_start - _root->system_page_size, _root->system_page_size, MADV_DONTNEED);
    munmap(zone->user_pages_start + ZONE_USER_SIZE, _root->system_page_size);
    madvise(zone->user_pages_start + ZONE_USER_SIZE, _root->system_page_size, MADV_DONTNEED);
}

INTERNAL_HIDDEN void _iso_alloc_destroy_zone(iso_alloc_zone_t *zone) {
    LOCK_ROOT();
    _iso_alloc_destroy_zone_unlocked(zone, true, false);
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN void _iso_alloc_destroy_zone_unlocked(iso_alloc_zone_t *zone, bool flush_caches, bool replace) {
    if(flush_caches == true) {
        /* We don't need a lock to clear the zone cache
         * but we do it here because we don't want another
         * thread to stick the zone we are about to delete
         * into the cache for later */
        clear_zone_cache();
        _flush_chunk_quarantine();
    }

    UNMASK_ZONE_PTRS(zone);
    UNPOISON_ZONE(zone);

    if(zone->internal == false) {
        /* This zone can be used again, we just need to wipe
         * any sensitive data from it and prime it for use */
        memset(zone->bitmap_start, 0x0, zone->bitmap_size);
        memset(zone->user_pages_start, 0x0, ZONE_USER_SIZE);

#if MEMORY_TAGGING
        /* Clear the memory tags */
        size_t s = ROUND_UP_PAGE((GET_CHUNK_COUNT(zone) * MEM_TAG_SIZE));
        uint8_t *_mtp = (zone->user_pages_start - _root->system_page_size - s);
        memset(_mtp, 0x0, s);
        mprotect_pages(_mtp, s, PROT_NONE);
        zone->tagged = false;
#endif

#if NEVER_REUSE_ZONES || FUZZ_MODE
        /* This will waste memory because we will never
         * unmap these pages, even in the destructor */
        mprotect_pages(zone->bitmap_start, zone->bitmap_size, PROT_NONE);
        mprotect_pages(zone->user_pages_start, ZONE_USER_SIZE, PROT_NONE);

        /* Make this zone unusable */
        memset(zone, 0x0, sizeof(iso_alloc_zone_t));
        zone->is_full = true;
#else
        zone->internal = true;
        zone->is_full = false;

        /* Reusing private zones has the potential for introducing
         * zone-use-after-free patterns. So we bootstrap the zone
         * from scratch here */
        create_canary_chunks(zone);

        fill_free_bit_slot_cache(zone);

        /* Prime the next_free_bit_slot member */
        get_next_free_bit_slot(zone);

        MASK_ZONE_PTRS(zone);
#endif
        /* If we are destroying the zone lets give the memory
         * back to the OS. It will still be available if we
         * try to use it */
        madvise(zone->bitmap_start, zone->bitmap_size, MADV_DONTNEED);
        madvise(zone->user_pages_start, ZONE_USER_SIZE, MADV_DONTNEED);
        POISON_ZONE(zone);
    } else {
        if(replace == true) {
            /* The only time we ever destroy a default non-private zone
             * is from the destructor so its safe unmap pages */
            int16_t zones_used = _root->zones_used;
            size_t size = zone->chunk_size;

            /* _iso_new_zone() will use _root->zones_used to place
             * the new zone at the correct index in _root->zones.
             * We will restore this value after the new zone has
             * been created */
            _root->zones_used = zone->index;
            _unmap_zone(zone);
            _iso_new_zone(size, true);
            _root->zones_used = zones_used;
        } else {
            _unmap_zone(zone);
        }
    }
}

__attribute__((destructor(LAST_DTOR))) void iso_alloc_dtor(void) {
    LOCK_ROOT();

    _flush_chunk_quarantine();

#if HEAP_PROFILER
    _iso_output_profile();
#endif

#if NO_ZERO_ALLOCATIONS
    munmap(_zero_alloc_page, g_page_size);
#endif

#if DEBUG && (LEAK_DETECTOR || MEM_USAGE)
    uint64_t mb = 0;

    for(uint32_t i = 0; i < _root->zones_used; i++) {
        iso_alloc_zone_t *zone = &_root->zones[i];
        _iso_alloc_zone_leak_detector(zone, false);
    }

    mb = __iso_alloc_mem_usage();

#if MEM_USAGE
    LOG("Total megabytes consumed by all zones: %lu", mb);
    _iso_alloc_print_stats();
#endif

#endif

    for(uint32_t i = 0; i < _root->zones_used; i++) {
        iso_alloc_zone_t *zone = &_root->zones[i];
        _verify_zone(zone);
#if ISO_DTOR_CLEANUP
        _iso_alloc_destroy_zone_unlocked(zone, false, false);
#endif
    }

#if ISO_DTOR_CLEANUP
    /* Unmap all zone structures */
    munmap((void *) ((uintptr_t) _root->zones - g_page_size), _root->zones_size);
#endif

    iso_alloc_big_zone_t *big_zone = _root->big_zone_head;
    iso_alloc_big_zone_t *big = NULL;

    if(big_zone != NULL) {
        big_zone = UNMASK_BIG_ZONE_NEXT(_root->big_zone_head);
    }

    while(big_zone != NULL) {
        check_big_canary(big_zone);

        if(big_zone->next != NULL) {
            big = UNMASK_BIG_ZONE_NEXT(big_zone->next);
        } else {
            big = NULL;
        }

#if ISO_DTOR_CLEANUP
        /* Free the user pages first */
        void *up = big_zone->user_pages_start - _root->system_page_size;
        munmap(up, (_root->system_page_size << 1) + big_zone->size);

        /* Free the meta data */
        munmap(big_zone - _root->system_page_size, (_root->system_page_size * BIG_ZONE_META_DATA_PAGE_COUNT));
#endif
        big_zone = big;
    }

#if ISO_DTOR_CLEANUP
    munmap(_root->guard_below, _root->system_page_size);
    munmap(_root->guard_above, _root->system_page_size);
    munmap(_root, sizeof(iso_alloc_root));
    munmap(zone_lookup_table, ZONE_LOOKUP_TABLE_SZ);
    munmap(chunk_lookup_table, CHUNK_TO_ZONE_TABLE_SZ);

#if !THREAD_SUPPORT
    munmap(chunk_quarantine - (g_page_size / sizeof(uintptr_t)), ROUND_UP_PAGE(CHUNK_QUARANTINE_SZ * sizeof(uintptr_t)) + (g_page_size * 2));
    munmap(zone_cache - g_page_size, ROUND_UP_PAGE(ZONE_CACHE_SZ * sizeof(_tzc)) + g_page_size * 2);
#endif

#endif
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN iso_alloc_zone_t *iso_new_zone(size_t size, bool internal) {
    if(size > SMALL_SZ_MAX) {
        return NULL;
    }

    LOCK_ROOT();
    iso_alloc_zone_t *zone = _iso_new_zone(size, internal);
    UNLOCK_ROOT();
    return zone;
}

/* Requires the root is locked */
INTERNAL_HIDDEN iso_alloc_zone_t *_iso_new_zone(size_t size, bool internal) {
    if(UNLIKELY(_root->zones_used >= MAX_ZONES)) {
        LOG_AND_ABORT("Cannot allocate additional zones. I have already allocated %d", _root->zones_used);
    }

    /* In order for our bitmap to be a power of 2
     * the size we allocate also needs to be. We
     * want our bitmap to be a power of 2 because
     * if its not then we either waste memory
     * or have to perform inefficient searches
     * whenever we need more bitslots */
    if((is_pow2(size)) != true) {
        size = next_pow2(size);
    }

    if(size > SMALL_SZ_MAX) {
        LOG("Request for new zone with %ld byte chunks should be handled by big alloc path", size);
        return NULL;
    }

    /* Minimum chunk size */
    if(size < SMALLEST_CHUNK_SZ) {
        size = SMALLEST_CHUNK_SZ;
    }

    iso_alloc_zone_t *new_zone = &_root->zones[_root->zones_used];

    memset(new_zone, 0x0, sizeof(iso_alloc_zone_t));

    new_zone->internal = internal;
    new_zone->is_full = false;
    new_zone->chunk_size = size;

    size_t chunk_count = GET_CHUNK_COUNT(new_zone);

    /* If a caller requests an allocation that is >=(ZONE_USER_SIZE/2)
     * then we need to allocate a minimum size bitmap */
    uint32_t bitmap_size = (chunk_count << BITS_PER_CHUNK_SHIFT) >> BITS_PER_BYTE_SHIFT;
    new_zone->bitmap_size = (bitmap_size > sizeof(bitmap_index_t)) ? bitmap_size : sizeof(bitmap_index_t);

    /* All of the following fields are immutable
     * and should not change once they are set */
    void *p = mmap_rw_pages(new_zone->bitmap_size + (_root->system_page_size << 1), true, ZONE_BITMAP_NAME);

    void *bitmap_pages_guard_below = p;
    new_zone->bitmap_start = (p + _root->system_page_size);

    void *bitmap_pages_guard_above = (void *) ROUND_UP_PAGE((uintptr_t) p + (new_zone->bitmap_size + _root->system_page_size));

    create_guard_page(bitmap_pages_guard_below);
    create_guard_page(bitmap_pages_guard_above);

    /* Bitmap pages are accessed often and usually in sequential order */
    madvise(new_zone->bitmap_start, new_zone->bitmap_size, MADV_WILLNEED);

    char *name = NULL;

#if NAMED_MAPPINGS && __ANDROID__
    if(internal == true) {
        name = INTERNAL_UZ_NAME;
    } else {
        name = PRIVATE_UZ_NAME;
    }
#endif

    size_t total_size = ZONE_USER_SIZE + (_root->system_page_size << 1);

#if MEMORY_TAGGING
    /* Each tag is 1 byte in size and the start address
     * of each valid chunk is assigned a tag */
    size_t tag_mapping_size = ROUND_UP_PAGE((GET_CHUNK_COUNT(new_zone) * MEM_TAG_SIZE));

    if(internal == false) {
        total_size += (tag_mapping_size + g_page_size);
        new_zone->tagged = true;
    } else {
        tag_mapping_size = 0;
    }
#endif
    /* All user pages use MAP_POPULATE. This might seem like we are asking
     * the kernel to commit a lot of memory for us that we may never use
     * but when we call create_canary_chunks() that will happen anyway */
    p = mmap_rw_pages(total_size, false, name);

#if NAMED_MAPPINGS && __ANDROID__
    if(new_zone->tagged == false) {
        name = MEM_TAG_NAME;
    }
#endif

    void *user_pages_guard_below = p;
    create_guard_page(user_pages_guard_below);

#if MEMORY_TAGGING
    if(new_zone->tagged == true) {
        create_guard_page(p + _root->system_page_size + tag_mapping_size);
        new_zone->user_pages_start = (p + _root->system_page_size + tag_mapping_size + _root->system_page_size);

        uint64_t *_mtp = p + _root->system_page_size;

        /* Generate random tags */
        for(uint64_t o = 0; o < tag_mapping_size / sizeof(uint64_t); o++) {
            _mtp[o] = rand_uint64();
        }
    } else {
        new_zone->user_pages_start = (p + _root->system_page_size);
    }
#else
    new_zone->user_pages_start = (p + _root->system_page_size);
#endif

    void *user_pages_guard_above;

#if MEMORY_TAGGING
    if(new_zone->tagged == false) {
        user_pages_guard_above = (void *) ROUND_UP_PAGE((uintptr_t) p + (ZONE_USER_SIZE + _root->system_page_size));
    } else {
        user_pages_guard_above = (void *) ROUND_UP_PAGE((uintptr_t) p + tag_mapping_size + (ZONE_USER_SIZE + _root->system_page_size * 2));
    }
#else
    user_pages_guard_above = (void *) ROUND_UP_PAGE((uintptr_t) p + (ZONE_USER_SIZE + _root->system_page_size));
#endif

    create_guard_page(user_pages_guard_above);

    madvise(new_zone->user_pages_start, ZONE_USER_SIZE, MADV_WILLNEED);

    new_zone->index = _root->zones_used;
    new_zone->canary_secret = rand_uint64();
    new_zone->pointer_mask = rand_uint64();

    create_canary_chunks(new_zone);

    /* When we create a new zone its an opportunity to
     * populate our free list cache with random entries */
    fill_free_bit_slot_cache(new_zone);

    /* Prime the next_free_bit_slot member */
    get_next_free_bit_slot(new_zone);

#if CPU_PIN
    new_zone->cpu_core = sched_getcpu();
#endif

    POISON_ZONE(new_zone);

    /* The lookup table is never used for private zones */
    if(LIKELY(internal == true)) {
        chunk_lookup_table[ADDR_TO_CHUNK_TABLE(new_zone->user_pages_start)] = new_zone->index;

        /* If no other zones of this size exist then set the
         * index in the zone lookup table to its index */
        if(zone_lookup_table[size] == 0) {
            zone_lookup_table[size] = new_zone->index;
        } else {
            /* Other zones exist that hold this size. We need to
             * fixup the most recent ones next_sz_index member.
             * We do this by walking the list using next_sz_index */
            for(int32_t i = zone_lookup_table[size]; i < _root->zones_used;) {
                iso_alloc_zone_t *zt = &_root->zones[i];

                if(zt->chunk_size != size) {
                    LOG_AND_ABORT("Inconsistent lookup table for zone[%d] chunk size %d (%d)", zt->index, zt->chunk_size, size);
                }

                /* Follow this zone's next_sz_index member */
                if(zt->next_sz_index != 0) {
                    i = zt->next_sz_index;
                } else {
                    /* If this zones next_sz_index is zero then set
                     * it to the zone we just created and break */
                    zt->next_sz_index = new_zone->index;
                    break;
                }
            }
        }
    }

    MASK_ZONE_PTRS(new_zone);

    _root->zones_used++;

    return new_zone;
}

/* Iterate through a zone bitmap a qword at a time
 * looking for empty holes (i.e. slot == 0) */
INTERNAL_HIDDEN bit_slot_t iso_scan_zone_free_slot(iso_alloc_zone_t *zone) {
    const bitmap_index_t *bm = (bitmap_index_t *) zone->bitmap_start;
    const bitmap_index_t max_bm_idx = GET_MAX_BITMASK_INDEX(zone);

    /* Iterate the entire bitmap a qword at a time */
    for(bitmap_index_t i = 0; i < max_bm_idx; i++) {
        /* If the byte is 0 then there are some free
         * slots we can use at this location */
        if(bm[i] == 0x0) {
            return (i << BITS_PER_QWORD_SHIFT);
        }
    }

    return BAD_BIT_SLOT;
}

/* This function scans an entire bitmap bit-by-bit
 * and returns the first free bit position. In a heavily
 * used zone this function will be slow to search. We
 * speed it up by looking for a constant ALLOCATED_BITSLOTS
 * that indicates there is at least 1 free bit slot  */
INTERNAL_HIDDEN bit_slot_t iso_scan_zone_free_slot_slow(iso_alloc_zone_t *zone) {
    const bitmap_index_t *bm = (bitmap_index_t *) zone->bitmap_start;
    const bitmap_index_t max_bm_idx = GET_MAX_BITMASK_INDEX(zone);

    for(bitmap_index_t i = 0; i < max_bm_idx; i++) {
        for(int64_t j = 0; j < BITS_PER_QWORD; j += BITS_PER_CHUNK) {
            /* We can easily check if every bitslot represented by
             * this qword is allocated with or without canaries */
            if(bm[i] < ALLOCATED_BITSLOTS) {
                if((GET_BIT(bm[i], j)) == 0) {
                    return ((i << BITS_PER_QWORD_SHIFT) + j);
                }
            }
        }
    }

    return BAD_BIT_SLOT;
}

INTERNAL_HIDDEN iso_alloc_zone_t *is_zone_usable(iso_alloc_zone_t *zone, size_t size) {
    /* If the zone is full it is not usable */
    if(zone->is_full == true) {
        return NULL;
    }

    /* This zone may fit this chunk but if the zone was
     * created for chunks more than (N * larger) than the
     * requested allocation size then we would be wasting
     * a lot of memory by using it. We only do this for
     * sizes beyond ZONE_1024 bytes. In other words we can
     * live with some wasted space in zones that manage
     * chunks smaller than ZONE_1024 */
    if(zone->internal == true && size > ZONE_1024 && zone->chunk_size >= (size << WASTED_SZ_MULTIPLIER_SHIFT)) {
        return NULL;
    }

    if(zone->next_free_bit_slot != BAD_BIT_SLOT) {
        return zone;
    }

    UNMASK_ZONE_PTRS(zone);

    /* If the cache for this zone is empty we should
     * refill it to make future allocations faster
     * for all threads */
    if(zone->free_bit_slot_cache_usable >= zone->free_bit_slot_cache_index) {
        fill_free_bit_slot_cache(zone);
    }

    bit_slot_t bit_slot = get_next_free_bit_slot(zone);

    if(LIKELY(bit_slot != BAD_BIT_SLOT)) {
        MASK_ZONE_PTRS(zone);
        return zone;
    }

    /* Free list failed, use a fast search */
    bit_slot = iso_scan_zone_free_slot(zone);

    if(UNLIKELY(bit_slot == BAD_BIT_SLOT)) {
        /* Fast search failed, search bit by bit */
        bit_slot = iso_scan_zone_free_slot_slow(zone);
        MASK_ZONE_PTRS(zone);

        /* This zone may be entirely full, try the next one
         * but mark this zone full so future allocations can
         * take a faster path */
        if(bit_slot == BAD_BIT_SLOT) {
            zone->is_full = true;
            return NULL;
        } else {
            zone->next_free_bit_slot = bit_slot;
            return zone;
        }
    } else {
        zone->next_free_bit_slot = bit_slot;
        MASK_ZONE_PTRS(zone);
        return zone;
    }
}

/* Implements the check for iso_find_zone_fit */
INTERNAL_HIDDEN bool iso_does_zone_fit(iso_alloc_zone_t *zone, size_t size) {
#if CPU_PIN
    if(zone->cpu_core != sched_getcpu()) {
        return false;
    }
#endif

    /* Don't return a zone that handles a size far larger
     * than we need. This could lead to high memory usage
     * depending on allocation patterns but helps enforce
     * spatial separation based on sized */
    if(zone->chunk_size >= ZONE_1024 && size <= ZONE_128) {
        return false;
    }

    if(zone->chunk_size < size || zone->internal == false || zone->is_full == true) {
        return false;
    }

    /* We found a zone, lets try to find a free slot in it */
    zone = is_zone_usable(zone, size);

    if(zone == NULL) {
        return false;
    } else {
        return true;
    }
}

/* Finds a zone that can fit this allocation request */
INTERNAL_HIDDEN iso_alloc_zone_t *iso_find_zone_fit(size_t size) {
    iso_alloc_zone_t *zone = NULL;
    int32_t i = 0;

    if(IS_ALIGNED(size) != 0) {
        size = ALIGN_SZ_UP(size);
    }

    /* Fast path via lookup table */
    if(zone_lookup_table[size] != 0) {
        i = zone_lookup_table[size];

        for(; i < _root->zones_used;) {
            iso_alloc_zone_t *zone = &_root->zones[i];

            if(zone->chunk_size != size) {
                LOG_AND_ABORT("Zone lookup table failed to match sizes for zone[%d](%d) for chunk size (%d)", zone->index, zone->chunk_size, size);
            }

            if(zone->internal == false) {
                LOG_AND_ABORT("Lookup table should never contain private zones");
            }

            bool fits = iso_does_zone_fit(zone, size);

            if(fits == true) {
                return zone;
            }

            if(zone->next_sz_index != 0) {
                i = zone->next_sz_index;
            } else {
                /* We have reached the end of our linked zones. The
                 * lookup table failed to find us a usable zone.
                 * Instead of creating a new one we will break out
                 * of this loop and try iterating through all zones,
                 * including ones we may have skipped over, to find
                 * a suitable candidate. */
                break;
            }
        }
    }

#if SMALL_MEM_STARTUP
    /* A simple optimization to find which default zone
     * should fit this allocation. If we fail then a
     * slower iterative approach is used. The longer a
     * program runs the more likely we will fail this
     * fast path as default zones may fill up */
    if(size >= ZONE_512 && size <= MAX_DEFAULT_ZONE_SZ) {
        i = DEFAULT_ZONE_COUNT >> 1;
    } else if(size > MAX_DEFAULT_ZONE_SZ) {
        i = DEFAULT_ZONE_COUNT;
    }
#else
    i = 0;
#endif

    for(; i < _root->zones_used; i++) {
        zone = &_root->zones[i];

        bool fits = iso_does_zone_fit(zone, size);

        if(fits == true) {
            return zone;
        }
    }

    return NULL;
}

INTERNAL_HIDDEN ASSUME_ALIGNED void *_iso_calloc(size_t nmemb, size_t size) {
    unsigned int res;
    size_t sz = nmemb * size;

    if(__builtin_umul_overflow(nmemb, size, &res)) {
        LOG_AND_ABORT("Call to calloc() will overflow nmemb=%zu size=%zu", nmemb, size);
        return NULL;
    }

    void *p = _iso_alloc(NULL, sz);

    memset(p, 0x0, sz);
    return p;
}

INTERNAL_HIDDEN void *_iso_big_alloc(size_t size) {
    const size_t new_size = ROUND_UP_PAGE(size);

    if(new_size < size || new_size > BIG_SZ_MAX) {
        LOG_AND_ABORT("Cannot allocate a big zone of %ld bytes", new_size);
    }

    size = new_size;

    LOCK_BIG_ZONE();

    /* Let's first see if theres an existing set of
     * pages that can satisfy this allocation request */
    iso_alloc_big_zone_t *big = _root->big_zone_head;

    if(big != NULL) {
        big = UNMASK_BIG_ZONE_NEXT(_root->big_zone_head);
    }

    iso_alloc_big_zone_t *last_big = NULL;

    while(big != NULL) {
        check_big_canary(big);

        if(big->free == true && big->size >= size) {
            break;
        }

        last_big = big;

        if(big->next != NULL) {
            big = UNMASK_BIG_ZONE_NEXT(big->next);
        } else {
            big = NULL;
            break;
        }
    }

    /* We need to setup a new set of pages */
    if(big == NULL) {
        /* User data is allocated separately from big zone meta
         * data to prevent an attacker from targeting it */
        void *user_pages = mmap_rw_pages((_root->system_page_size << BIG_ZONE_USER_PAGE_COUNT_SHIFT) + size, false, BIG_ZONE_UD_NAME);

        if(user_pages == NULL) {
            UNLOCK_BIG_ZONE();
#if ABORT_ON_NULL
            LOG_AND_ABORT("isoalloc configured to abort on NULL");
#endif
            return NULL;
        }

        void *p = mmap_rw_pages((_root->system_page_size * BIG_ZONE_META_DATA_PAGE_COUNT), false, BIG_ZONE_MD_NAME);

        /* The first page before meta data is a guard page */
        create_guard_page(p);

        /* The second page is for meta data and it is placed
         * at a random offset from the start of the page */
        big = (iso_alloc_big_zone_t *) (p + _root->system_page_size);
        madvise(big, _root->system_page_size, MADV_WILLNEED);
        uint32_t random_offset = ALIGN_SZ_DOWN(rand_uint64());
        size_t s = _root->system_page_size - (sizeof(iso_alloc_big_zone_t) - 1);

        big = (iso_alloc_big_zone_t *) ((p + _root->system_page_size) + ((random_offset * s) >> 32));
        big->free = false;
        big->size = size;
        big->next = NULL;

        if(last_big != NULL) {
            last_big->next = MASK_BIG_ZONE_NEXT(big);
        }

        if(_root->big_zone_head == NULL) {
            _root->big_zone_head = MASK_BIG_ZONE_NEXT(big);
        }

        /* Create the guard page after the meta data */
        void *next_gp = (p + (_root->system_page_size << 1));
        create_guard_page(next_gp);

        /* The first page is a guard page */
        create_guard_page(user_pages);

        /* Tell the kernel we want to access this big zone allocation */
        user_pages += _root->system_page_size;
        madvise(user_pages, size, MADV_WILLNEED);

        /* The last page beyond user data is a guard page */
        void *last_gp = (user_pages + size);
        create_guard_page(last_gp);

        /* Save a pointer to the user pages */
        big->user_pages_start = user_pages;

        /* The canaries prevents a linear overwrite of the big
         * zone meta data structure from either direction */
        big->canary_a = ((uint64_t) big ^ __builtin_bswap64((uint64_t) big->user_pages_start) ^ _root->big_zone_canary_secret);
        big->canary_b = big->canary_a;

        UNLOCK_BIG_ZONE();
        return big->user_pages_start;
    } else {
        check_big_canary(big);
        big->free = false;
        UNPOISON_BIG_ZONE(big);
        UNLOCK_BIG_ZONE();
        return big->user_pages_start;
    }
}

INTERNAL_HIDDEN ASSUME_ALIGNED void *_iso_alloc_bitslot_from_zone(bit_slot_t bitslot, iso_alloc_zone_t *zone) {
    const bitmap_index_t dwords_to_bit_slot = (bitslot >> BITS_PER_QWORD_SHIFT);
    const int64_t which_bit = WHICH_BIT(bitslot);

    void *p = POINTER_FROM_BITSLOT(zone, bitslot);
    UNPOISON_ZONE_CHUNK(zone, p);

#if !ENABLE_ASAN && !DISABLE_CANARY
    __builtin_prefetch(p, 1);
#endif

    bitmap_index_t *bm = (bitmap_index_t *) zone->bitmap_start;

    /* Read out 64 bits from the bitmap. We will write
     * them back before we return. This reduces the
     * number of times we have to hit the bitmap page
     * which could result in a page fault */
    bitmap_index_t b = bm[dwords_to_bit_slot];

    if(UNLIKELY(p > zone->user_pages_start + ZONE_USER_SIZE)) {
        LOG_AND_ABORT("Allocating an address 0x%p from zone[%d], bit slot %lu %ld bytes %ld pages outside zones user pages 0x%p 0x%p",
                      p, zone->index, bitslot, p - (zone->user_pages_start + ZONE_USER_SIZE), (p - (zone->user_pages_start + ZONE_USER_SIZE)) / _root->system_page_size,
                      zone->user_pages_start, zone->user_pages_start + ZONE_USER_SIZE);
    }

    if(UNLIKELY((GET_BIT(b, which_bit)) != 0)) {
        LOG_AND_ABORT("Zone[%d] for chunk size %d cannot return allocated chunk at 0x%p bitmap location @ 0x%p. bit slot was %lu, bit number was %lu",
                      zone->index, zone->chunk_size, p, &bm[dwords_to_bit_slot], bitslot, which_bit);
    }

    /* This chunk was either previously allocated and free'd
     * or it's a canary chunk. In either case this means it
     * has a canary written in its first dword. Here we check
     * that canary and abort if its been corrupted */
#if !ENABLE_ASAN && !DISABLE_CANARY
    if((GET_BIT(b, (which_bit + 1))) == 1) {
        check_canary(zone, p);
        *(uint64_t *) p = 0x0;
    }
#endif

    /* Set the in-use bit */
    SET_BIT(b, which_bit);

    /* The second bit is flipped to 0 while in use. This
     * is because a previously in use chunk would have
     * a bit pattern of 11 which makes it looks the same
     * as a canary chunk. This bit is set again upon free */
    UNSET_BIT(b, (which_bit + 1));
    bm[dwords_to_bit_slot] = b;
    zone->af_count++;
    zone->alloc_count++;
    return p;
}

/* Populates the thread cache, requires the root is locked and zone is unmasked */
INTERNAL_HIDDEN INLINE void populate_zone_cache(iso_alloc_zone_t *zone) {
    if(UNLIKELY(zone->internal == false)) {
        return;
    }

    /* Don't cache this zone if it was recently cached */
    if(zone_cache_count != 0 && zone_cache[zone_cache_count - 1].zone == zone) {
        return;
    }

    if(zone_cache_count < ZONE_CACHE_SZ) {
        zone_cache[zone_cache_count].zone = zone;
        zone_cache[zone_cache_count].chunk_size = zone->chunk_size;
        zone_cache_count++;
    } else {
        zone_cache_count = 0;
        zone_cache[zone_cache_count].zone = zone;
        zone_cache[zone_cache_count].chunk_size = zone->chunk_size;
    }
}

INTERNAL_HIDDEN uint8_t _iso_alloc_get_mem_tag(void *p, iso_alloc_zone_t *zone) {
#if MEMORY_TAGGING
    void *user_pages_start = UNMASK_USER_PTR(zone);

    uint8_t *_mtp = (user_pages_start - _root->system_page_size - ROUND_UP_PAGE((GET_CHUNK_COUNT(zone) * MEM_TAG_SIZE)));
    const uint64_t chunk_offset = (uint64_t) (p - user_pages_start);

    /* Ensure the pointer is a multiple of chunk size */
    if(UNLIKELY((chunk_offset & (zone->chunk_size - 1)) != 0)) {
        LOG_AND_ABORT("Chunk offset %d not an alignment of %d", chunk_offset, zone->chunk_size);
    }

    _mtp += (chunk_offset / zone->chunk_size);
    return *_mtp;
#else
    return 0;
#endif
}

INTERNAL_HIDDEN void *_tag_ptr(void *p, iso_alloc_zone_t *zone) {
    if(UNLIKELY(p == NULL || zone == NULL)) {
        return NULL;
    }

    const uint64_t tag = _iso_alloc_get_mem_tag(p, zone);
    return (void *) ((tag << UNTAGGED_BITS) | (uintptr_t) p);
}

INTERNAL_HIDDEN void *_untag_ptr(void *p, iso_alloc_zone_t *zone) {
    if(UNLIKELY(p == NULL || zone == NULL)) {
        return NULL;
    }

    void *untagged_p = (void *) ((uintptr_t) p & TAGGED_PTR_MASK);
    const uint64_t tag = _iso_alloc_get_mem_tag(untagged_p, zone);
    return (void *) ((tag << UNTAGGED_BITS) ^ (uintptr_t) p);
}

INTERNAL_HIDDEN ASSUME_ALIGNED void *_iso_alloc(iso_alloc_zone_t *zone, size_t size) {
#if NO_ZERO_ALLOCATIONS
    if(UNLIKELY(size == 0 && _root != NULL)) {
        return _zero_alloc_page;
    }
#endif

    if(UNLIKELY(zone && size > zone->chunk_size)) {
        LOG_AND_ABORT("Private zone %d cannot hold chunks of size %d", zone->index, zone->chunk_size);
    }

    LOCK_ROOT();

    if(UNLIKELY(_root == NULL)) {
        if(UNLIKELY(zone != NULL)) {
            LOG_AND_ABORT("_root was NULL but zone %p was not", zone);
        }

        g_page_size = sysconf(_SC_PAGESIZE);
        iso_alloc_initialize_global_root();

#if NO_ZERO_ALLOCATIONS
        /* In the unlikely event size is 0 but we hadn't
         * initialized the root yet return the zero page */
        if(UNLIKELY(size == 0)) {
            UNLOCK_ROOT();
            return _zero_alloc_page;
        }
#endif
    }

#if ALLOC_SANITY
    /* We don't sample if we are allocating from a private zone */
    if(zone != NULL) {
        /* We only sample allocations smaller than an individual
         * page. We are unlikely to find uninitialized reads on
         * larger size and it makes tracking them less complex */
        const size_t sampled_size = ALIGN_SZ_UP(size);

        if(sampled_size < _root->system_page_size && _sane_sampled < MAX_SANE_SAMPLES) {
            /* If we chose to sample this allocation then
             * _iso_alloc_sample will call UNLOCK_ROOT() */
            void *ps = _iso_alloc_sample(sampled_size);

            if(ps != NULL) {
                return ps;
            }
        }
    }
#endif

#if HEAP_PROFILER
    _iso_alloc_profile(size);
#endif

    /* Allocation requests of SMALL_SZ_MAX bytes or larger are
     * handled by the 'big allocation' path. If a zone was
     * passed in we abort because its a misuse of the API */
    if(LIKELY(size <= SMALL_SZ_MAX)) {
#if FUZZ_MODE
        _verify_all_zones();
#endif
        if(LIKELY(zone == NULL)) {
            /* Hot Path: Check the zone cache for a zone this
             * thread recently used for an alloc/free operation.
             * It's likely we are allocating a similar size chunk
             * and this will speed up that operation */
            for(int64_t i = 0; i < zone_cache_count; i++) {
                if(zone_cache[i].chunk_size >= size) {
                    bool fit = iso_does_zone_fit(zone_cache[i].zone, size);

                    if(fit == true) {
                        zone = zone_cache[i].zone;
                        break;
                    }
                }
            }
        }

        bit_slot_t free_bit_slot = BAD_BIT_SLOT;

        /* Slow Path: This will iterate through all zones
         * looking for a suitable one, this includes the
         * zones we cached above */
        if(zone == NULL) {
            zone = iso_find_zone_fit(size);
        }

        /* We only need to check if the zone is usable
         * if it's a private zone. If we chose this zone
         * then its guaranteed to already be usable */
        if(LIKELY(zone != NULL)) {
            if(zone->internal == false) {
                zone = is_zone_usable(zone, size);

                if(zone == NULL) {
                    UNLOCK_ROOT();
#if ABORT_ON_NULL
                    LOG_AND_ABORT("isoalloc configured to abort on NULL");
#endif
                    return NULL;
                }
            }

            free_bit_slot = zone->next_free_bit_slot;

            if(UNLIKELY(free_bit_slot == BAD_BIT_SLOT)) {
                UNLOCK_ROOT();
#if ABORT_ON_NULL
                LOG_AND_ABORT("isoalloc configured to abort on NULL");
#endif
                return NULL;
            }
        } else {
            /* Extra Slow Path: We need a new zone in order
             * to satisfy this allocation request */
            zone = _iso_new_zone(size, true);

            if(UNLIKELY(zone == NULL)) {
                LOG_AND_ABORT("Failed to create a zone for allocation of %zu bytes", size);
            }

            /* This is a brand new zone, so the fast path
             * should always work. Abort if it doesn't */
            free_bit_slot = zone->next_free_bit_slot;

            if(UNLIKELY(free_bit_slot == BAD_BIT_SLOT)) {
                LOG_AND_ABORT("Allocated a new zone with no free bit slots");
            }
        }

        UNMASK_ZONE_PTRS(zone);

        zone->next_free_bit_slot = BAD_BIT_SLOT;
        void *p = _iso_alloc_bitslot_from_zone(free_bit_slot, zone);

        MASK_ZONE_PTRS(zone);
        UNLOCK_ROOT();

        /* Zones internal status cannot be converted, it's
         * safe to access this bool after unlocking and
         * then populating the zone cache */
        if(zone->internal == false) {
            populate_zone_cache(zone);
        }

        return p;
    } else {
        /* It's safe to unlock the root at this point because
         * the big zone allocation path uses a different lock */
        UNLOCK_ROOT();

        if(UNLIKELY(zone != NULL)) {
            LOG_AND_ABORT("Allocation size of %d is > %d and cannot use a private zone", size, SMALL_SZ_MAX);
        }

        return _iso_big_alloc(size);
    }
}

INTERNAL_HIDDEN iso_alloc_big_zone_t *iso_find_big_zone(void *p) {
    LOCK_BIG_ZONE();

    /* Its possible we are trying to unmap a big allocation */
    iso_alloc_big_zone_t *big_zone = _root->big_zone_head;

    if(big_zone != NULL) {
        big_zone = UNMASK_BIG_ZONE_NEXT(_root->big_zone_head);
    }

    while(big_zone != NULL) {
        check_big_canary(big_zone);

        /* Only a free of the exact address is valid */
        if(p == big_zone->user_pages_start) {
            UNLOCK_BIG_ZONE();
            return big_zone;
        }

        if(UNLIKELY(p > big_zone->user_pages_start && p < (big_zone->user_pages_start + big_zone->size))) {
            LOG_AND_ABORT("Invalid free of big zone allocation at 0x%p in mapping 0x%p", p, big_zone->user_pages_start);
        }

        if(big_zone->next != NULL) {
            big_zone = UNMASK_BIG_ZONE_NEXT(big_zone->next);
        } else {
            big_zone = NULL;
            break;
        }
    }

    UNLOCK_BIG_ZONE();
    return NULL;
}

/* iso_find_zone_bitmap_range and iso_find_zone_range are
 * logically identical functions that both return a zone
 * for a pointer. The only difference is where the pointer
 * addresses, a bitmap or user pages */
INTERNAL_HIDDEN iso_alloc_zone_t *iso_find_zone_bitmap_range(const void *restrict p) {
    iso_alloc_zone_t *zone = NULL;

    /* The chunk lookup table is the fastest way to find a
     * zone given a pointer to a chunk so we check it first */
    uint16_t zone_index = chunk_lookup_table[ADDR_TO_CHUNK_TABLE(p)];

    if(UNLIKELY(zone_index > _root->zones_used)) {
        LOG_AND_ABORT("Pointer to zone lookup table corrupted at position %zu", ADDR_TO_CHUNK_TABLE(p));
    }

    zone = &_root->zones[zone_index];
    void *bitmap_start = UNMASK_BITMAP_PTR(zone);

    if(LIKELY(bitmap_start <= p && (bitmap_start + zone->bitmap_size) > p)) {
        return zone;
    }

    iso_alloc_zone_t *tmp_zone = NULL;

    /* Now we check the MRU thread zone cache */
    for(int64_t i = 0; i < zone_cache_count; i++) {
        tmp_zone = zone_cache[i].zone;
        bitmap_start = UNMASK_BITMAP_PTR(tmp_zone);

        if(bitmap_start <= p && (bitmap_start + tmp_zone->bitmap_size) > p) {
            return tmp_zone;
        }
    }

    /* Now we check all zones, this is the slowest path */
    for(int64_t i = 0; i < _root->zones_used; i++) {
        zone = &_root->zones[i];

        bitmap_start = UNMASK_BITMAP_PTR(zone);

        if(bitmap_start <= p && (bitmap_start + zone->bitmap_size) > p) {
            return zone;
        }
    }

    return NULL;
}

INTERNAL_HIDDEN iso_alloc_zone_t *iso_find_zone_range(const void *restrict p) {
    iso_alloc_zone_t *zone = NULL;

    /* The chunk lookup table is the fastest way to find a
     * zone given a pointer to a chunk so we check it first */
    uint16_t zone_index = chunk_lookup_table[ADDR_TO_CHUNK_TABLE(p)];

    if(UNLIKELY(zone_index > _root->zones_used)) {
        LOG_AND_ABORT("Pointer to zone lookup table corrupted at position %zu", ADDR_TO_CHUNK_TABLE(p));
    }

    zone = &_root->zones[zone_index];
    void *user_pages_start = UNMASK_USER_PTR(zone);

    if(LIKELY(user_pages_start <= p && (user_pages_start + ZONE_USER_SIZE) > p)) {
        return zone;
    }

    iso_alloc_zone_t *tmp_zone = NULL;

    /* Now we check the MRU thread zone cache */
    for(int64_t i = 0; i < zone_cache_count; i++) {
        tmp_zone = zone_cache[i].zone;
        user_pages_start = UNMASK_USER_PTR(tmp_zone);

        if(user_pages_start <= p && (user_pages_start + ZONE_USER_SIZE) > p) {
            return tmp_zone;
        }
    }

    /* Now we check all zones, this is the slowest path */
    for(int64_t i = 0; i < _root->zones_used; i++) {
        zone = &_root->zones[i];

        user_pages_start = UNMASK_USER_PTR(zone);

        if(user_pages_start <= p && (user_pages_start + ZONE_USER_SIZE) > p) {
            return zone;
        }
    }

    return NULL;
}

/* Checking canaries under ASAN mode is not trivial. ASAN
 * provides a strong guarantee that these chunks haven't
 * been modified in some way */
#if ENABLE_ASAN || DISABLE_CANARY
INTERNAL_HIDDEN INLINE void check_big_canary(iso_alloc_big_zone_t *big) {
    return;
}

INTERNAL_HIDDEN INLINE void write_canary(iso_alloc_zone_t *zone, const void *p) {
    return;
}

/* Verify the canary value in an allocation */
INTERNAL_HIDDEN INLINE void check_canary(iso_alloc_zone_t *zone, const void *p) {
    return;
}

INTERNAL_HIDDEN int64_t check_canary_no_abort(iso_alloc_zone_t *zone, const void *p) {
    return OK;
}
#else
/* Verifies both canaries in a big zone structure. This
 * is a fast operation so we call it anytime we iterate
 * through the linked list of big zones */
INTERNAL_HIDDEN INLINE void check_big_canary(iso_alloc_big_zone_t *big) {
    const uint64_t canary = ((uint64_t) big ^ __builtin_bswap64((uint64_t) big->user_pages_start) ^ _root->big_zone_canary_secret);

    if(UNLIKELY(big->canary_a != canary)) {
        LOG_AND_ABORT("Big zone 0x%p bottom canary has been corrupted! Value: 0x%x Expected: 0x%x", big, big->canary_a, canary);
    }

    if(UNLIKELY(big->canary_b != canary)) {
        LOG_AND_ABORT("Big zone 0x%p top canary has been corrupted! Value: 0x%x Expected: 0x%x", big, big->canary_a, canary);
    }
}

/* All free chunks get a canary written at both
 * the start and end of their chunks. These canaries
 * are verified when adjacent chunks are allocated,
 * freed, or when the API requests validation. We
 * sacrifice the high byte in entropy to prevent
 * unbounded string reads from leaking it */
INTERNAL_HIDDEN INLINE void write_canary(iso_alloc_zone_t *zone, const void *p) {
    const uint64_t canary = (zone->canary_secret ^ (uint64_t) p) & CANARY_VALIDATE_MASK;
    *(uint64_t *) p = canary;
    p += (zone->chunk_size - sizeof(uint64_t));
    *(uint64_t *) p = canary;
}

/* Verify the canary value in an allocation */
INTERNAL_HIDDEN INLINE void check_canary(iso_alloc_zone_t *zone, const void *p) {
    uint64_t v = *((uint64_t *) p);
    const uint64_t canary = (zone->canary_secret ^ (uint64_t) p) & CANARY_VALIDATE_MASK;

    if(UNLIKELY(v != canary)) {
        LOG_AND_ABORT("Canary at beginning of chunk 0x%p in zone[%d][%d byte chunks] has been corrupted! Value: 0x%x Expected: 0x%x",
                      p, zone->index, zone->chunk_size, v, canary);
    }

    v = *((uint64_t *) (p + zone->chunk_size - sizeof(uint64_t)));

    if(UNLIKELY(v != canary)) {
        LOG_AND_ABORT("Canary at end of chunk 0x%p in zone[%d][%d byte chunks] has been corrupted! Value: 0x%x Expected: 0x%x",
                      p, zone->index, zone->chunk_size, v, canary);
    }
}

INTERNAL_HIDDEN int64_t check_canary_no_abort(iso_alloc_zone_t *zone, const void *p) {
    uint64_t v = *((uint64_t *) p);
    const uint64_t canary = (zone->canary_secret ^ (uint64_t) p) & CANARY_VALIDATE_MASK;

    if(UNLIKELY(v != canary)) {
        LOG("Canary at beginning of chunk 0x%p in zone[%d] has been corrupted! Value: 0x%x Expected: 0x%x", p, zone->index, v, canary);
        return ERR;
    }

    v = *((uint64_t *) (p + zone->chunk_size - sizeof(uint64_t)));

    if(UNLIKELY(v != canary)) {
        LOG("Canary at end of chunk 0x%p in zone[%d] has been corrupted! Value: 0x%x Expected: 0x%x", p, zone->index, v, canary);
        return ERR;
    }

    return OK;
}
#endif

INTERNAL_HIDDEN void iso_free_big_zone(iso_alloc_big_zone_t *big_zone, bool permanent) {
    LOCK_BIG_ZONE();
    if(UNLIKELY(big_zone->free == true)) {
        LOG_AND_ABORT("Double free of big zone 0x%p has been detected!", big_zone);
    }

#if !ENABLE_ASAN && SANITIZE_CHUNKS
    memset(big_zone->user_pages_start, POISON_BYTE, big_zone->size);
#endif

    madvise(big_zone->user_pages_start, big_zone->size, MADV_DONTNEED);

    /* If this isn't a permanent free then all we need
     * to do is sanitize the mapping and mark it free.
     * The pages backing the big zone can be reused. */
    if(LIKELY(permanent == false)) {
        POISON_BIG_ZONE(big_zone);
        big_zone->free = true;
    } else {
        iso_alloc_big_zone_t *big = _root->big_zone_head;

        if(big != NULL) {
            big = UNMASK_BIG_ZONE_NEXT(_root->big_zone_head);
        }

        if(big == big_zone) {
            _root->big_zone_head = NULL;
        } else {
            /* We need to remove this entry from the list */
            while(big != NULL) {
                check_big_canary(big);

                if(UNMASK_BIG_ZONE_NEXT(big->next) == big_zone) {
                    big->next = UNMASK_BIG_ZONE_NEXT(big_zone->next);
                    break;
                }

                if(big->next != NULL) {
                    big = UNMASK_BIG_ZONE_NEXT(big->next);
                } else {
                    big = NULL;
                }
            }
        }

        if(UNLIKELY(big == NULL)) {
            LOG_AND_ABORT("The big zone list has been corrupted, unable to find big zone 0x%p", big_zone);
        }

        mprotect_pages(big_zone->user_pages_start, big_zone->size, PROT_NONE);
        memset(big_zone, POISON_BYTE, sizeof(iso_alloc_big_zone_t));

        /* Big zone meta data is at a random offset from its base page */
        mprotect_pages(((void *) ROUND_DOWN_PAGE((uintptr_t) big_zone)), _root->system_page_size, PROT_NONE);
    }

    UNLOCK_BIG_ZONE();
}

INTERNAL_HIDDEN void iso_free_chunk_from_zone(iso_alloc_zone_t *zone, void *restrict p, bool permanent) {
    /* Ensure the pointer is properly aligned */
    if(UNLIKELY(IS_ALIGNED((uintptr_t) p) != 0)) {
        LOG_AND_ABORT("Chunk at 0x%p of zone[%d] is not %d byte aligned", p, zone->index, ALIGNMENT);
    }

    const uint64_t chunk_offset = (uint64_t) (p - UNMASK_USER_PTR(zone));

    /* Ensure the pointer is a multiple of chunk size */
    if(UNLIKELY((chunk_offset & (zone->chunk_size - 1)) != 0)) {
        LOG_AND_ABORT("Chunk at 0x%p is not a multiple of zone[%d] chunk size %d. Off by %lu bits",
                      p, zone->index, zone->chunk_size, (chunk_offset & (zone->chunk_size - 1)));
    }

    const size_t chunk_number = (chunk_offset / zone->chunk_size);
    const bit_slot_t bit_slot = (chunk_number << BITS_PER_CHUNK_SHIFT);
    const bit_slot_t dwords_to_bit_slot = (bit_slot >> BITS_PER_QWORD_SHIFT);

    if(UNLIKELY(dwords_to_bit_slot > (zone->bitmap_size >> 3))) {
        LOG_AND_ABORT("Cannot calculate this chunks location in the bitmap 0x%p", p);
    }

    uint64_t which_bit = WHICH_BIT(bit_slot);
    bitmap_index_t *bm = (bitmap_index_t *) UNMASK_BITMAP_PTR(zone);

    /* Read out 64 bits from the bitmap. We will write
     * them back before we return. This reduces the
     * number of times we have to hit the bitmap page
     * which could result in a page fault */
    bitmap_index_t b = bm[dwords_to_bit_slot];

    /* Double free detection */
    if(UNLIKELY((GET_BIT(b, which_bit)) == 0)) {
        LOG_AND_ABORT("Double free of chunk 0x%p detected from zone[%d] dwords_to_bit_slot=%lu bit_slot=%lu",
                      p, zone->index, dwords_to_bit_slot, bit_slot);
    }

    /* Set the next bit so we know this chunk was used */
    SET_BIT(b, (which_bit + 1));

#if !ENABLE_ASAN && (!DISABLE_CANARY || SANITIZE_CHUNKS)
    __builtin_prefetch(p, 1);
#endif

    /* Unset the bit and write the value into the bitmap
     * if this is not a permanent free. A permanent free
     * means this chunk will be marked as if it is a canary */
    if(LIKELY(permanent == false)) {
        UNSET_BIT(b, which_bit);
        insert_free_bit_slot(zone, bit_slot);
        zone->is_full = false;
#if !ENABLE_ASAN && SANITIZE_CHUNKS
        iso_clear_user_chunk(p, zone->chunk_size);
#endif
    } else {
        iso_clear_user_chunk(p, zone->chunk_size);
    }

    bm[dwords_to_bit_slot] = b;

    zone->af_count--;

    /* Now that we have free'd this chunk lets validate the
     * chunks before and after it. If they were previously
     * used and currently free they should have canaries
     * we can verify */
#if !ENABLE_ASAN && !DISABLE_CANARY
    write_canary(zone, p);

    if((chunk_number + 1) != GET_CHUNK_COUNT(zone)) {
        const bit_slot_t bit_slot_over = ((chunk_number + 1) << BITS_PER_CHUNK_SHIFT);
        if((GET_BIT(bm[(bit_slot_over >> BITS_PER_QWORD_SHIFT)], (WHICH_BIT(bit_slot_over) + 1))) == 1) {
            check_canary(zone, p + zone->chunk_size);
        }
    }

    if(chunk_number != 0) {
        const bit_slot_t bit_slot_under = ((chunk_number - 1) << BITS_PER_CHUNK_SHIFT);
        if((GET_BIT(bm[(bit_slot_under >> BITS_PER_QWORD_SHIFT)], (WHICH_BIT(bit_slot_under) + 1))) == 1) {
            check_canary(zone, p - zone->chunk_size);
        }
    }
#endif

    POISON_ZONE_CHUNK(zone, p);
    populate_zone_cache(zone);
}

INTERNAL_HIDDEN void _iso_free_from_zone(void *p, iso_alloc_zone_t *zone, bool permanent) {
    if(p == NULL) {
        return;
    }

#if MEMORY_TAGGING
    /* Its possible that we were passed a tagged pointer */
    if(UNLIKELY(zone != NULL && zone->tagged == true && ((uintptr_t) p & IS_TAGGED_PTR_MASK) != 0)) {
        /* If the untagging results in a bad pointer we
         * will catch it in the free path and abort */
        p = _untag_ptr(p, zone);
    }
#endif

    LOCK_ROOT();
    _iso_free_internal_unlocked(p, permanent, zone);
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN INLINE void clear_chunk_quarantine() {
#if THREAD_SUPPORT
    memset(chunk_quarantine, 0x0, sizeof(chunk_quarantine));
#else
    memset(chunk_quarantine, 0x0, CHUNK_QUARANTINE_SZ * sizeof(uintptr_t));
#endif

    chunk_quarantine_count = 0;
}

INTERNAL_HIDDEN INLINE void clear_zone_cache() {
#if THREAD_SUPPORT
    memset(zone_cache, 0x0, sizeof(zone_cache));
#else
    memset(zone_cache, 0x0, ZONE_CACHE_SZ * sizeof(_tzc));
#endif

    zone_cache_count = 0;
}

INTERNAL_HIDDEN void _iso_free(void *p, bool permanent) {
    if(p == NULL) {
        return;
    }

#if NO_ZERO_ALLOCATIONS
    if(p == _zero_alloc_page) {
        return;
    }
#endif

#if ALLOC_SANITY
    int32_t r = _iso_alloc_free_sane_sample(p);

    if(r == OK) {
        return;
    }
#endif

#if HEAP_PROFILER
    _iso_free_profile();
#endif

    if(permanent == true) {
        _iso_free_internal(p, permanent);
        return;
    }

    if(chunk_quarantine_count < CHUNK_QUARANTINE_SZ) {
        chunk_quarantine[chunk_quarantine_count] = (uintptr_t) p;
        chunk_quarantine_count++;
        return;
    } else {
        for(int64_t i = 0; i < chunk_quarantine_count; i++) {
            _iso_free_internal((void *) chunk_quarantine[i], false);
        }

        clear_chunk_quarantine();
        chunk_quarantine[chunk_quarantine_count] = (uintptr_t) p;
        chunk_quarantine_count++;
        return;
    }
}

INTERNAL_HIDDEN void _iso_free_size(void *p, size_t size) {
    if(p == NULL) {
        return;
    }

#if NO_ZERO_ALLOCATIONS
    if(p == _zero_alloc_page && size != 0) {
        LOG_AND_ABORT("Zero sized chunk (0x%p) with non-zero (%d) size passed to free", p, size);
    }

    if(p == _zero_alloc_page) {
        return;
    }
#endif

#if ALLOC_SANITY
    int32_t r = _iso_alloc_free_sane_sample(p);

    if(r == OK) {
        return;
    }
#endif

    if(UNLIKELY(size > SMALL_SZ_MAX)) {
        iso_alloc_big_zone_t *big_zone = iso_find_big_zone(p);

        if(UNLIKELY(big_zone == NULL)) {
            LOG_AND_ABORT("Could not find any zone for allocation at 0x%p", p);
        }

        iso_free_big_zone(big_zone, false);
        return;
    }

    LOCK_ROOT();

    iso_alloc_zone_t *zone = iso_find_zone_range(p);

    if(UNLIKELY(zone == NULL)) {
        LOG_AND_ABORT("Could not find zone for %p", p);
    }

    /* We can't check for an exact size match because
     * its possible we chose a larger zone to hold this
     * chunk when we allocated it */
    if(UNLIKELY(zone->chunk_size < size)) {
        LOG_AND_ABORT("Invalid size (expected %d, got %d) for chunk 0x%p", zone->chunk_size, size, p);
    }

    _iso_free_internal_unlocked(p, false, zone);

    UNLOCK_ROOT();
}

INTERNAL_HIDDEN void _iso_free_internal(void *p, bool permanent) {
    LOCK_ROOT();
    _iso_free_internal_unlocked(p, permanent, NULL);
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN bool _is_zone_retired(iso_alloc_zone_t *zone) {
    /* If the zone has no active allocations, holds smaller chunks,
     * and has allocated and freed more than ZONE_ALLOC_RETIRE
     * chunks in its lifetime then we destroy and replace it with
     * a new zone */
    if(UNLIKELY(zone->af_count == 0 && zone->alloc_count > (GET_CHUNK_COUNT(zone) * ZONE_ALLOC_RETIRE))) {
        if(zone->internal == true && zone->chunk_size < (MAX_DEFAULT_ZONE_SZ * 2)) {
            return true;
        }
    }

    return false;
}

INTERNAL_HIDDEN bool _refresh_zone_mem_tags(iso_alloc_zone_t *zone) {
#if MEMORY_TAGGING
    if(UNLIKELY(zone->af_count == 0 && zone->alloc_count > ((GET_CHUNK_COUNT(zone) * ZONE_ALLOC_RETIRE)) / 4)) {
        size_t s = ROUND_UP_PAGE((GET_CHUNK_COUNT(zone) * MEM_TAG_SIZE));
        uint64_t *_mtp = (zone->user_pages_start - _root->system_page_size - s);

        for(uint64_t o = 0; o > s / sizeof(uint64_t); o++) {
            _mtp[o] = rand_uint64();
        }

        return true;
    }
#endif
    return false;
}

INTERNAL_HIDDEN void _iso_free_internal_unlocked(void *p, bool permanent, iso_alloc_zone_t *zone) {
#if FUZZ_MODE
    _verify_all_zones();
#endif

    if(LIKELY(zone == NULL)) {
        zone = iso_find_zone_range(p);
    }

    if(LIKELY(zone != NULL)) {
        iso_free_chunk_from_zone(zone, p, permanent);

        /* If the zone has no active allocations, holds smaller chunks,
         * and has allocated and freed more than ZONE_ALLOC_RETIRE
         * chunks in its lifetime then we destroy and replace it with
         * a new zone */
        if(UNLIKELY(_is_zone_retired(zone))) {
            _iso_alloc_destroy_zone_unlocked(zone, false, true);
        }

#if MEMORY_TAGGING
        /* If there are no chunks allocated but this zone has seen
         * %25 of ZONE_ALLOC_RETIRE in allocations we wipe the pointer
         * tags and start fresh. If the whole zone doesn't need to
         * be refreshed then just generate a new tag for this chunk */
        if(zone->tagged == true) {
            if(_refresh_zone_mem_tags(zone) == false) {
                /* We only need to refresh this single tag */
                if(zone->tagged == true) {
                    void *user_pages_start = UNMASK_USER_PTR(zone);
                    uint8_t *_mtp = (user_pages_start - _root->system_page_size - ROUND_UP_PAGE((GET_CHUNK_COUNT(zone) * MEM_TAG_SIZE)));
                    uint64_t chunk_offset = (uint64_t) (p - user_pages_start);
                    _mtp += (chunk_offset / zone->chunk_size);

                    /* Generate and write a new tag for this chunk */
                    uint8_t mem_tag = (uint8_t) rand_uint64();
                    *_mtp = mem_tag;
                }
            }
        }
#endif

#if UAF_PTR_PAGE
        if(UNLIKELY((rand_uint64() % UAF_PTR_PAGE_ODDS) == 1)) {
            _iso_alloc_ptr_search(p, true);
        }
#endif
    } else {
        iso_alloc_big_zone_t *big_zone = iso_find_big_zone(p);

        if(UNLIKELY(big_zone == NULL)) {
            LOG_AND_ABORT("Could not find any zone for allocation at 0x%p", p);
        }

        iso_free_big_zone(big_zone, permanent);
    }
}

/* Disable all use of iso_alloc by protecting the _root */
INTERNAL_HIDDEN void _iso_alloc_protect_root(void) {
    LOCK_ROOT();
    mprotect_pages(_root, sizeof(iso_alloc_root), PROT_NONE);
}

/* Unprotect all use of iso_alloc by allowing R/W of the _root */
INTERNAL_HIDDEN void _iso_alloc_unprotect_root(void) {
    mprotect_pages(_root, sizeof(iso_alloc_root), PROT_READ | PROT_WRITE);
    UNLOCK_ROOT();
}

INTERNAL_HIDDEN size_t _iso_chunk_size(void *p) {
    if(p == NULL) {
        return 0;
    }

#if NO_ZERO_ALLOCATIONS
    if(UNLIKELY(p == _zero_alloc_page)) {
        return 0;
    }
#endif

#if ALLOC_SANITY
    LOCK_SANITY_CACHE();
    _sane_allocation_t *sane_alloc = _get_sane_alloc(p);

    if(sane_alloc != NULL) {
        size_t orig_size = sane_alloc->orig_size;
        UNLOCK_SANITY_CACHE();
        return orig_size;
    }

    UNLOCK_SANITY_CACHE();
#endif

    LOCK_ROOT();

    /* We cannot return NULL here, we abort instead */
    iso_alloc_zone_t *zone = iso_find_zone_range(p);

    if(UNLIKELY(zone == NULL)) {
        UNLOCK_ROOT();
        iso_alloc_big_zone_t *big_zone = iso_find_big_zone(p);

        if(UNLIKELY(big_zone == NULL)) {
            LOG_AND_ABORT("Could not find any zone for allocation at 0x%p", p);
        }

        return big_zone->size;
    }

    UNLOCK_ROOT();
    return zone->chunk_size;
}

INTERNAL_HIDDEN uint64_t _iso_alloc_detect_leaks_in_zone(iso_alloc_zone_t *zone) {
    LOCK_ROOT();
    uint64_t leaks = _iso_alloc_zone_leak_detector(zone, false);
    UNLOCK_ROOT();
    return leaks;
}

INTERNAL_HIDDEN uint64_t _iso_alloc_mem_usage() {
    LOCK_ROOT();
    uint64_t mem_usage = __iso_alloc_mem_usage();
    mem_usage += _iso_alloc_big_zone_mem_usage();
    UNLOCK_ROOT();
    return mem_usage;
}

INTERNAL_HIDDEN uint64_t _iso_alloc_big_zone_mem_usage() {
    LOCK_BIG_ZONE();
    uint64_t mem_usage = __iso_alloc_big_zone_mem_usage();
    UNLOCK_BIG_ZONE();
    return mem_usage;
}

INTERNAL_HIDDEN uint64_t _iso_alloc_zone_mem_usage(iso_alloc_zone_t *zone) {
    LOCK_ROOT();
    uint64_t zone_mem_usage = __iso_alloc_zone_mem_usage(zone);
    UNLOCK_ROOT();
    return zone_mem_usage;
}

#if UNIT_TESTING
/* Some tests require getting access to IsoAlloc internals
 * that aren't supported by the API. We never want these
 * in release builds of the library */
EXTERNAL_API iso_alloc_root *_get_root(void) {
    return _root;
}
#endif
