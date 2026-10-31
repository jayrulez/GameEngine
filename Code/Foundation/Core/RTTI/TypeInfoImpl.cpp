// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :type_info implementation unit
//
// The process-single TypeInfo slot table behind TypeOf<T>(). It is defined here, not
// inline, so there is one definition in Core: every image that instantiates TypeOf<T>()
// (each with its own cached reference) resolves to the same TypeInfo, and metadata
// patched by one library's registrars is what every other library reads.
//
// The table lives in static storage. The index and the slot pool are zero-initialized
// .bss (untouched pages cost nothing), so the table is usable from the first TypeOf<T>()
// call in static initialization, needs no allocator, and has no destructor to order
// against static destruction. An image that embeds its own Core (the static-engine test
// plugin, an unsupported model that a test dlcloses) also leaves nothing on the heap
// behind it for LeakSanitizer to report. Only a process with more
// value types than the pool holds grows onto the heap, and that memory is never freed:
// a slot must outlive its first registrar's image (a hot-reloaded module finds its
// slots already there and re-patches them).

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

module foundation.core;

namespace foundation::core::detail
{
    namespace
    {
        // Sizing: a few hundred TypeOf<T>() instantiations exist (plus container and
        // handle instantiations); 4096 slots is an order of magnitude of headroom before
        // the first heap chunk. The index runs open addressing at <= 50% load.
        constexpr usize kStaticIndexCapacity = 16384; // power of two
        constexpr usize kPoolChunkCapacity = 4096;

        struct IndexEntry
        {
            TypeId key = 0; // 0 = empty (SignatureTypeId never yields 0)
            TypeInfo* slot = nullptr;
        };

        struct PoolChunk
        {
            TypeInfo slots[kPoolChunkCapacity];
            usize used = 0;
            PoolChunk* next = nullptr;
        };

        SpinLock g_lock; // constant-initialized: safe before any constructor has run
        IndexEntry g_staticIndex[kStaticIndexCapacity];
        PoolChunk g_staticPool;

        IndexEntry* g_index = g_staticIndex;
        usize g_indexCapacity = kStaticIndexCapacity;
        usize g_indexUsed = 0;
        PoolChunk* g_pool = &g_staticPool;

        [[nodiscard]] usize Probe(TypeId key, usize capacity) noexcept
        {
            return static_cast<usize>(key) & (capacity - 1);
        }

        void Insert(IndexEntry* index, usize capacity, TypeId key, TypeInfo* slot) noexcept
        {
            usize i = Probe(key, capacity);
            while (index[i].key != 0)
            {
                i = (i + 1) & (capacity - 1);
            }
            index[i].key = key;
            index[i].slot = slot;
        }

        // Past the static index (a pathological count of value types): rehash into a heap
        // array twice the size. This is process-lifetime state, so it uses the default
        // allocator. Every probe runs under g_lock, so a heap-allocated predecessor can be
        // freed at once.
        void GrowIndex() noexcept
        {
            const usize capacity = g_indexCapacity * 2;
            auto* grown = static_cast<IndexEntry*>(
                DefaultAllocator().Allocate(capacity * sizeof(IndexEntry), alignof(IndexEntry)));
            for (usize i = 0; i < capacity; ++i)
            {
                new (&grown[i]) IndexEntry{};
            }
            for (usize i = 0; i < g_indexCapacity; ++i)
            {
                if (g_index[i].key != 0)
                {
                    Insert(grown, capacity, g_index[i].key, g_index[i].slot);
                }
            }
            IndexEntry* previous = g_index;
            g_index = grown;
            g_indexCapacity = capacity;
            if (previous != g_staticIndex)
            {
                DefaultAllocator().Free(previous);
            }
        }

        [[nodiscard]] TypeInfo* AllocateSlot() noexcept
        {
            if (g_pool->used == kPoolChunkCapacity)
            {
                PoolChunk* chunk = DefaultAllocator().New<PoolChunk>();
                chunk->next = g_pool;
                g_pool = chunk;
            }
            return &g_pool->slots[g_pool->used++];
        }
    }

    TypeInfo& TypeInfoSlot(TypeId signatureId, const TypeInfo& prototype) noexcept
    {
        DIAGNOSTIC_ASSERT_MSG(signatureId != 0, "signature ids are never 0");
        g_lock.Lock();
        TypeInfo* slot = nullptr;
        usize i = Probe(signatureId, g_indexCapacity);
        for (;;)
        {
            IndexEntry& entry = g_index[i];
            if (entry.key == signatureId)
            {
                slot = entry.slot;
                // Layout facts are prototype-derived only, and a rebuilt module (hot reload)
                // may legitimately change them. Everything a registrar patched stays.
                slot->size = prototype.size;
                slot->align = prototype.align;
                break;
            }
            if (entry.key == 0)
            {
                slot = AllocateSlot();
                *slot = prototype;
                entry.key = signatureId;
                entry.slot = slot;
                if (++g_indexUsed * 2 > g_indexCapacity)
                {
                    GrowIndex();
                }
                break;
            }
            i = (i + 1) & (g_indexCapacity - 1);
        }
        g_lock.Unlock();
        return *slot;
    }
}
