// Draconic Foundation - :allocator partition (foundation)
//
// The allocator vocabulary every consumer needs: alignment / raw-memory
// helpers, Construct/Destruct, the IAllocator interface, the default
// SystemAllocator, and the per-allocation header used by wrapping allocators.
// Specialized allocators (Linear/Stack/Pool/Frame/Tracking/Tagged) live in
// their own partitions and build on this.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <cstddef> // std::max_align_t
#include <cstdlib> // aligned_alloc / free
#include <cstring> // memcpy / memmove / memset
#include <new>     // placement new

export module draconic.foundation:allocator;

import :base;

export namespace draconic::foundation
{
    // =======================================================================
    // Alignment helpers
    // =======================================================================
    [[nodiscard]] constexpr bool IsPowerOfTwo(usize value) noexcept
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    [[nodiscard]] constexpr usize AlignUp(usize value, usize alignment) noexcept
    {
        // alignment must be a power of two.
        return (value + (alignment - 1)) & ~(alignment - 1);
    }

    [[nodiscard]] constexpr usize AlignDown(usize value, usize alignment) noexcept
    {
        return value & ~(alignment - 1);
    }

    [[nodiscard]] inline bool IsAligned(const void* pointer, usize alignment) noexcept
    {
        return (reinterpret_cast<usize>(pointer) & (alignment - 1)) == 0;
    }

    inline constexpr usize kDefaultAlignment = alignof(std::max_align_t);

    // =======================================================================
    // Raw memory operations (thin, named wrappers over the C library)
    // =======================================================================
    inline void* MemCopy(void* dst, const void* src, usize bytes) noexcept
    {
        return std::memcpy(dst, src, bytes);
    }

    inline void* MemMove(void* dst, const void* src, usize bytes) noexcept
    {
        return std::memmove(dst, src, bytes);
    }

    inline void* MemSet(void* dst, i32 value, usize bytes) noexcept
    {
        return std::memset(dst, value, bytes);
    }

    // 0 when the two regions hold identical bytes; nonzero otherwise (std::memcmp sign).
    [[nodiscard]] inline i32 MemCompare(const void* a, const void* b, usize bytes) noexcept
    {
        return std::memcmp(a, b, bytes);
    }

    inline void MemZero(void* dst, usize bytes) noexcept { std::memset(dst, 0, bytes); }

    // =======================================================================
    // Placement construct / destroy
    //   Centralizing placement-new here (where <new> is included) keeps the
    //   global placement operator new reachable: container modules call these
    //   instead of `::new`, so consumers that instantiate containers never need
    //   to include <new> themselves. (GCC modules require it at the
    //   instantiation site otherwise.)
    // =======================================================================
    template <typename T, typename... Args>
    T* Construct(void* where, Args&&... args)
    {
        return ::new (where) T(Forward<Args>(args)...);
    }

    template <typename T>
    void Destruct(T* object) noexcept
    {
        object->~T();
    }

    // =======================================================================
    // Allocator interface
    //   Allocate returns nullptr on failure (no exceptions).
    //   Free(nullptr) is a no-op.
    // =======================================================================
    class IAllocator
    {
    public:
        virtual ~IAllocator() = default;

        [[nodiscard]] virtual void* Allocate(usize size, usize alignment = kDefaultAlignment) = 0;
        virtual void Free(void* pointer) = 0;

        // Construct/destroy a single object through this allocator.
        template <typename T, typename... Args>
        [[nodiscard]] T* New(Args&&... args)
        {
            void* memory = Allocate(sizeof(T), alignof(T));
            if (memory == nullptr)
            {
                return nullptr;
            }
            return ::new (memory) T(Forward<Args>(args)...);
        }

        template <typename T>
        void Delete(T* pointer)
        {
            if (pointer == nullptr)
            {
                return;
            }
            pointer->~T();
            Free(pointer);
        }
    };

    // =======================================================================
    // SystemAllocator - aligned heap allocations from the OS/CRT.
    // =======================================================================
    class SystemAllocator final : public IAllocator
    {
    public:
        [[nodiscard]] void* Allocate(usize size, usize alignment = kDefaultAlignment) override
        {
            if (size == 0)
            {
                return nullptr;
            }

            DRACONIC_ASSERT(IsPowerOfTwo(alignment));

            // POSIX only promises alignments that are multiples of sizeof(void*): glibc's
            // aligned_alloc happens to accept less, emscripten's REJECTS it (alignment 1
            // returned null on wasm - byte-aligned arrays like HashMap states). Raising
            // alignment is always safe.
            if (alignment < sizeof(void*))
            {
                alignment = sizeof(void*);
            }

            // aligned_alloc requires the size to be a multiple of the alignment.
            const usize alignedSize = AlignUp(size, alignment);
#if DRACONIC_PLATFORM_WINDOWS
            return _aligned_malloc(alignedSize, alignment);
#else
            return std::aligned_alloc(alignment, alignedSize);
#endif
        }

        void Free(void* pointer) override
        {
#if DRACONIC_PLATFORM_WINDOWS
            _aligned_free(pointer);
#else
            std::free(pointer);
#endif
        }
    };

    // Process-wide default heap allocator.
    [[nodiscard]] IAllocator& DefaultAllocator() noexcept
    {
        static SystemAllocator instance;
        return instance;
    }

    // A per-allocation header lets a wrapping allocator recover the original
    // size/base from just the user pointer on Free.
    namespace detail
    {
        struct AllocHeader
        {
            usize size;
            void* base;
        };

        // Allocates `size` bytes from `backing` behind a header; returns the
        // user pointer (aligned to >= alignment), or nullptr.
        [[nodiscard]] inline void* AllocWithHeader(IAllocator& backing, usize size, usize alignment)
        {
            const usize effectiveAlign =
                (alignment >= alignof(AllocHeader)) ? alignment : alignof(AllocHeader);
            const usize prefix = AlignUp(sizeof(AllocHeader), effectiveAlign);
            void* base = backing.Allocate(prefix + size, effectiveAlign);
            if (base == nullptr)
            {
                return nullptr;
            }
            byte* user = static_cast<byte*>(base) + prefix;
            auto* header = reinterpret_cast<AllocHeader*>(user - sizeof(AllocHeader));
            header->size = size;
            header->base = base;
            return user;
        }

        // Frees a headered allocation; returns the size that was recorded.
        inline usize FreeWithHeader(IAllocator& backing, void* user)
        {
            auto* header =
                reinterpret_cast<AllocHeader*>(static_cast<byte*>(user) - sizeof(AllocHeader));
            const usize size = header->size;
            backing.Free(header->base);
            return size;
        }
    }
}
