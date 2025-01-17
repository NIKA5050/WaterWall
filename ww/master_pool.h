#pragma once

#include "hmutex.h"
#include "utils/mathutils.h"
#include "ww.h"

/*
    Master Pool

    In some cases, workers need to send data/buffers to each other, while each have a thread local pool

    therefore, thread local pools may keep running out of items, and there is a need for a thread-safe-pool

    thread local pools will fall back to the master pool instead of allocating more memory or freeing it and
    interacting with os, malloc,free; in a single batch allocation for a full charge with only 1 mutex lock


                                |-----------|
                                |           |
                                |           |
                                |           | -------------> |----------------|
                                |           |                | Worker 1 pools |
                                |           | <------------- |----------------|
                                |           |
                                |           |
                                |           |
                                |           |
                                |           | -------------> |----------------|
                                |           |                | Worker 2 pools |
                                |           | <------------- |----------------|
     |-----------|              |           |
     |  Malloc   | -----------> |  MASTER   |
     | --------  |              |           |
     |   Free    | <----------- |   POOL    |
     |-----------|              |           |
                                |           | -------------> |----------------|
                                |           |                | Worker 3 pools |
                                |           | <------------- |----------------|
                                |           |
                                |           |
                                |           |
                                |           |
                                |           | -------------> |----------------|
                                |           |                | Worker 4 pools |
                                |           | <------------- |----------------|
                                |           |
                                |           |
                                |-----------|


*/

struct master_pool_s;
typedef void master_pool_item_t;

// pool handles are assumed to be thread safe
typedef master_pool_item_t *(*MasterPoolItemCreateHandle)(struct master_pool_s *pool, void *userdata);
typedef void (*MasterPoolItemDestroyHandle)(struct master_pool_s *pool, master_pool_item_t *item, void *userdata);

/*
    do not read this pool properties from the struct, its a multi-threaded object
*/

typedef struct master_pool_s
{
    void                       *memptr;
    hmutex_t              mutex;
    MasterPoolItemCreateHandle  create_item_handle;
    MasterPoolItemDestroyHandle destroy_item_handle;
    atomic_uint                 len;
    const unsigned int          cap;
    void                       *available[];
} ATTR_ALIGNED_LINE_CACHE master_pool_t;

static inline void popMasterPoolItems(master_pool_t *const pool, master_pool_item_t const **const iptr,
                                      const unsigned int count, void *userdata)
{
    // for (unsigned int i = 0; i < count; i++)
    // {
    //     iptr[i] = pool->create_item_handle(pool, userdata);
    // }
    // return;
    unsigned int i = 0;

    if (atomic_load_explicit(&(pool->len), memory_order_relaxed) > 0)
    {
        hmutex_lock(&(pool->mutex));
        const unsigned int tmp_len  = atomic_load_explicit(&(pool->len), memory_order_relaxed);
        const unsigned int consumed = min(tmp_len, count);

        if (consumed > 0)
        {
            atomic_fetch_add_explicit(&(pool->len), -consumed, memory_order_relaxed);
            const unsigned int pbase = (tmp_len - consumed);
            for (; i < consumed; i++)
            {
                iptr[i] = pool->available[pbase + i];
            }
        }
        hmutex_unlock(&(pool->mutex));
    }

    for (; i < count; i++)
    {
        iptr[i] = pool->create_item_handle(pool, userdata);
    }
}

static inline void reuseMasterPoolItems(master_pool_t *const pool, master_pool_item_t **const iptr,
                                        const unsigned int count, void *userdata)
{
    // for (unsigned int i = 0; i < count; i++)
    // {
    //     pool->destroy_item_handle(pool, iptr[i], userdata);
    // }
    // return;

    if (pool->cap == atomic_load_explicit(&(pool->len), memory_order_relaxed))
    {
        for (unsigned int i = 0; i < count; i++)
        {
            pool->destroy_item_handle(pool, iptr[i], userdata);
        }
        return;
    }

    unsigned int i = 0;

    hmutex_lock(&(pool->mutex));

    const unsigned int tmp_len  = atomic_load_explicit(&(pool->len), memory_order_relaxed);
    const unsigned int consumed = min(pool->cap - tmp_len, count);

    atomic_fetch_add_explicit(&(pool->len), consumed, memory_order_relaxed);

    for (; i < consumed; i++)
    {
        pool->available[i + tmp_len] = iptr[i];
    }

    hmutex_unlock(&(pool->mutex));

    for (; i < count; i++)
    {
        pool->destroy_item_handle(pool, iptr[i], userdata);
    }
}

static void installMasterPoolAllocCallbacks(master_pool_t *pool, MasterPoolItemCreateHandle create_h,
                                            MasterPoolItemDestroyHandle destroy_h)
{
    hmutex_lock(&(pool->mutex));
    pool->create_item_handle  = create_h;
    pool->destroy_item_handle = destroy_h;
    hmutex_unlock(&(pool->mutex));
}

master_pool_t *newMasterPoolWithCap(unsigned int pool_width);
