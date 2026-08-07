// Draconic Foundation - :memory_tag partition
//
// Memory tagging: attribute allocations to a named category via an open
// registry (Foundation does not enumerate subsystems). TaggedAllocator records
// per-tag byte/allocation totals. Thread-safe counters.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <atomic>

export module draconic.foundation:memory_tag;

import :base;
import :allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // Memory tagging - attribute allocations to a category. Foundation does NOT
    // enumerate subsystems (that would couple it to higher layers); instead it
    // hands out opaque tags by name via a small registry. Higher layers do:
    //     static const MemoryTag kGraphics = RegisterMemoryTag("Graphics");
    // Register tags at startup (single-threaded); allocation-time counters are
    // thread-safe.
    // =======================================================================
    struct MemoryTag
    {
        u32 value = 0;
    };

    inline constexpr MemoryTag kDefaultMemoryTag{0};

    namespace detail
    {
        inline constexpr usize kMaxMemoryTags = 64;

        struct MemoryTagRegistry
        {
            const char* names[kMaxMemoryTags]{};
            std::atomic<u64> bytes[kMaxMemoryTags]{};
            std::atomic<u64> counts[kMaxMemoryTags]{};
            std::atomic<u32> registered{1}; // slot 0 reserved for Default

            MemoryTagRegistry() { names[0] = "Default"; }
        };

        [[nodiscard]] inline MemoryTagRegistry& MemoryTags() noexcept
        {
            static MemoryTagRegistry registry;
            return registry;
        }

        [[nodiscard]] inline bool TagNameEquals(const char* a, const char* b) noexcept
        {
            usize i = 0;
            while (a[i] != '\0' && a[i] == b[i])
            {
                ++i;
            }
            return a[i] == b[i];
        }
    }

    // Returns a stable tag for `name`, creating it on first use (idempotent by
    // name). Falls back to the Default tag if the registry is full.
    [[nodiscard]] inline MemoryTag RegisterMemoryTag(const char* name)
    {
        detail::MemoryTagRegistry& registry = detail::MemoryTags();
        const u32 count = registry.registered.load(std::memory_order_acquire);
        for (u32 i = 0; i < count; ++i)
        {
            if (registry.names[i] != nullptr && detail::TagNameEquals(registry.names[i], name))
            {
                return MemoryTag{i};
            }
        }
        const u32 index = registry.registered.fetch_add(1, std::memory_order_acq_rel);
        if (index >= detail::kMaxMemoryTags)
        {
            DRACONIC_ASSERT_MSG(false, "Memory tag registry full");
            return kDefaultMemoryTag;
        }
        registry.names[index] = name;
        return MemoryTag{index};
    }

    [[nodiscard]] inline const char* MemoryTagName(MemoryTag tag) noexcept
    {
        const detail::MemoryTagRegistry& registry = detail::MemoryTags();
        return (tag.value < registry.registered.load(std::memory_order_relaxed))
                   ? registry.names[tag.value]
                   : "?";
    }

    [[nodiscard]] inline u32 MemoryTagCount() noexcept
    {
        return detail::MemoryTags().registered.load(std::memory_order_relaxed);
    }

    [[nodiscard]] inline u64 MemoryTagBytes(MemoryTag tag) noexcept
    {
        return detail::MemoryTags().bytes[tag.value].load(std::memory_order_relaxed);
    }
    [[nodiscard]] inline u64 MemoryTagAllocations(MemoryTag tag) noexcept
    {
        return detail::MemoryTags().counts[tag.value].load(std::memory_order_relaxed);
    }

    // Wraps an allocator and records per-tag byte/allocation totals. Thread-safe.
    class TaggedAllocator final : public IAllocator
    {
    public:
        TaggedAllocator(IAllocator& backing, MemoryTag tag) noexcept
            : m_backing(&backing), m_tag(tag.value)
        {
        }

        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            void* user = detail::AllocWithHeader(*m_backing, size, alignment);
            if (user != nullptr)
            {
                detail::MemoryTags().bytes[m_tag].fetch_add(size, std::memory_order_relaxed);
                detail::MemoryTags().counts[m_tag].fetch_add(1, std::memory_order_relaxed);
            }
            return user;
        }

        void Free(void* pointer) override
        {
            if (pointer != nullptr)
            {
                const usize size = detail::FreeWithHeader(*m_backing, pointer);
                detail::MemoryTags().bytes[m_tag].fetch_sub(size, std::memory_order_relaxed);
                detail::MemoryTags().counts[m_tag].fetch_sub(1, std::memory_order_relaxed);
            }
        }

    private:
        IAllocator* m_backing;
        u32 m_tag;
    };
}
