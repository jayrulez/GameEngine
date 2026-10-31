// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :ref_counted partition
//
// Intrusive strong+weak reference counting (no std:: equivalent). One
// mechanism: a co-allocated RefControl shared by the object and its weak refs.
//
//   RefCounted    - intrusive strong+weak base (Object derives from it).
//   RefPtr<T>     - strong shared ownership of a RefCounted-derived type.
//   WeakRefPtr<T> - non-owning weak reference; Lock() promotes to RefPtr.
//
// Lifetime model (std::shared_ptr semantics, single allocation):
//   * strong = number of RefPtr owners.
//   * weak   = number of WeakRefPtr owners + (1 while strong > 0).
//   * strong -> 0 destroys the object (runs ~T); the storage is retained.
//   * weak   -> 0 frees the storage.
// Allocation is explicit: the owning allocator is passed to MakeRef, matching
// the engine-wide policy. (Depends on :allocator only for IAllocator.)

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include <atomic>
#include <type_traits>

export module foundation.core:ref_counted;

import :base;
import :allocator;

namespace foundation::core::detail
{
    struct RefControl
    {
        mutable std::atomic<u32> strong;
        mutable std::atomic<u32> weak;
        IAllocator* allocator;
        void (*destroyObject)(void*) noexcept; // runs the object's destructor
        void* object;                          // the managed T*
        void* allocation;                      // base of the combined allocation
    };

    // MakeRef parks the (fully initialized) control here just before constructing the
    // object; RefCounted's ctor adopts it. This makes MemoryAllocator() valid inside
    // constructors, so a view can create its children from its own tree's allocator while
    // it is still being built. Thread-local: concurrent MakeRefs never cross wires, and
    // nested MakeRef calls consume their own slot before the outer ctor body continues.
    // The accessor is not inline (defined in RefCountedImpl.cpp): MakeRef<T> instantiates
    // in the calling library while the ctor may live in another, and a vague-linkage
    // thread_local would be duplicated per shared library, so the handshake would miss at
    // the boundary.
    [[nodiscard]] RefControl*& PendingRefControl() noexcept;

    inline void ReleaseWeak(RefControl* control) noexcept
    {
        if (control->weak.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            IAllocator* allocator = control->allocator;
            void* allocation = control->allocation;
            Destruct(control);
            if (allocator != nullptr)
            {
                allocator->Free(allocation);
            }
        }
    }
}

export namespace foundation::core
{
    template <typename T>
    class RefPtr;
    template <typename T>
    class WeakRefPtr;

    template <typename T, typename... Args>
    [[nodiscard]] RefPtr<T> MakeRef(IAllocator& allocator, Args&&... args);

    struct AdoptRef
    {
    }; // tag: take ownership of an already-counted reference

    // =======================================================================
    // RefCounted - intrusive strong+weak base. Heap-allocate via MakeRef.
    // =======================================================================
    class RefCounted
    {
    public:
        void AddRef() const noexcept
        {
            DIAGNOSTIC_ASSERT(m_control != nullptr);
            m_control->strong.fetch_add(1, std::memory_order_relaxed);
        }

        void Release() const noexcept
        {
            DIAGNOSTIC_ASSERT(m_control != nullptr);
            detail::RefControl* control = m_control;
            if (control->strong.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                // Last strong owner: destroy the object (this runs ~T, which
                // also ends this RefCounted subobject's lifetime, but `control`
                // lives independently), then drop the "alive" weak ref.
                control->destroyObject(control->object);
                detail::ReleaseWeak(control);
            }
        }

        [[nodiscard]] u32 RefCount() const noexcept
        {
            DIAGNOSTIC_ASSERT(m_control != nullptr);
            return m_control->strong.load(std::memory_order_relaxed);
        }

        /// The allocator this object was created from (recorded by MakeRef). Code inside a
        /// ref-counted object allocates its children from its own allocator
        /// (`MakeRef<Child>(MemoryAllocator())`), so the choice made at the tree's root
        /// threads through the whole tree with no parameter plumbing.
        [[nodiscard]] IAllocator& MemoryAllocator() const noexcept
        {
            DIAGNOSTIC_ASSERT(m_control != nullptr && m_control->allocator != nullptr);
            return *m_control->allocator;
        }

    protected:
        RefCounted() noexcept
            : m_control(detail::PendingRefControl())
        {
            detail::PendingRefControl() = nullptr;
        }
        virtual ~RefCounted() = default;

        RefCounted(const RefCounted&) = delete;
        RefCounted& operator=(const RefCounted&) = delete;

    private:
        template <typename U, typename... Args>
        friend RefPtr<U> MakeRef(IAllocator&, Args&&...);
        template <typename U>
        friend class WeakRefPtr;

        [[nodiscard]] detail::RefControl* Control() const noexcept { return m_control; }

        detail::RefControl* m_control = nullptr;
    };

    // =======================================================================
    // RefPtr - strong intrusive shared pointer.
    // =======================================================================
    template <typename T>
    class RefPtr
    {
    public:
        RefPtr() noexcept = default;
        RefPtr(decltype(nullptr)) noexcept {}

        explicit RefPtr(T* pointer) noexcept : m_ptr(pointer)
        {
            if (m_ptr != nullptr)
            {
                m_ptr->AddRef();
            }
        }

        // Adopt an already-incremented strong reference (no extra AddRef).
        RefPtr(T* pointer, AdoptRef) noexcept : m_ptr(pointer) {}

        RefPtr(const RefPtr& other) noexcept : m_ptr(other.m_ptr)
        {
            if (m_ptr != nullptr)
            {
                m_ptr->AddRef();
            }
        }

        RefPtr(RefPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

        template <typename U>
            requires std::is_convertible_v<U*, T*>
        RefPtr(const RefPtr<U>& other) noexcept : m_ptr(other.Get())
        {
            if (m_ptr != nullptr)
            {
                m_ptr->AddRef();
            }
        }

        ~RefPtr()
        {
            if (m_ptr != nullptr)
            {
                m_ptr->Release();
            }
        }

        RefPtr& operator=(const RefPtr& other) noexcept
        {
            if (this != &other)
            {
                if (other.m_ptr != nullptr)
                {
                    other.m_ptr->AddRef();
                }
                if (m_ptr != nullptr)
                {
                    m_ptr->Release();
                }
                m_ptr = other.m_ptr;
            }
            return *this;
        }

        RefPtr& operator=(RefPtr&& other) noexcept
        {
            if (this != &other)
            {
                if (m_ptr != nullptr)
                {
                    m_ptr->Release();
                }
                m_ptr = other.m_ptr;
                other.m_ptr = nullptr;
            }
            return *this;
        }

        void Reset() noexcept
        {
            if (m_ptr != nullptr)
            {
                m_ptr->Release();
            }
            m_ptr = nullptr;
        }

        [[nodiscard]] T* Get() const noexcept { return m_ptr; }
        [[nodiscard]] T* operator->() const noexcept { return m_ptr; }
        [[nodiscard]] T& operator*() const noexcept { return *m_ptr; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_ptr != nullptr; }

    private:
        T* m_ptr = nullptr;
    };

    // Free function template (not a hidden friend) so it gets COMDAT linkage
    // across module consumers rather than a strong per-TU symbol under GCC.
    template <typename T>
    [[nodiscard]] bool operator==(const RefPtr<T>& a, const RefPtr<T>& b) noexcept
    {
        return a.Get() == b.Get();
    }

    template <typename T, typename... Args>
    RefPtr<T> MakeRef(IAllocator& allocator, Args&&... args)
    {
        static_assert(std::is_base_of_v<RefCounted, T>,
                      "MakeRef requires a RefCounted-derived type.");

        constexpr usize alignment =
            (alignof(detail::RefControl) > alignof(T)) ? alignof(detail::RefControl) : alignof(T);
        const usize objectOffset = AlignUp(sizeof(detail::RefControl), alignof(T));
        const usize total = objectOffset + sizeof(T);

        void* base = allocator.Allocate(total, alignment);
        if (base == nullptr)
        {
            return RefPtr<T>{};
        }

        auto* control = Construct<detail::RefControl>(base);
        // Initialize the control before constructing the object and park it in the
        // thread-local slot: the RefCounted base ctor adopts it, so the object's own
        // constructor can already use MemoryAllocator() to build its children.
        control->strong.store(1, std::memory_order_relaxed);
        control->weak.store(1, std::memory_order_relaxed);
        control->allocator = &allocator;
        control->allocation = base;

        detail::PendingRefControl() = control;
        T* object = Construct<T>(static_cast<byte*>(base) + objectOffset, Forward<Args>(args)...);
        detail::PendingRefControl() = nullptr; // the base ctor consumed it; clear it regardless

        control->object = object;
        control->destroyObject = [](void* p) noexcept { Destruct(static_cast<T*>(p)); };

        static_cast<RefCounted*>(object)->m_control = control;
        return RefPtr<T>{object, AdoptRef{}};
    }

    // =======================================================================
    // WeakRefPtr - non-owning weak reference; Lock() promotes to RefPtr.
    // =======================================================================
    template <typename T>
    class WeakRefPtr
    {
    public:
        WeakRefPtr() noexcept = default;
        WeakRefPtr(decltype(nullptr)) noexcept {}

        WeakRefPtr(const RefPtr<T>& strong) noexcept
        {
            if (strong.Get() != nullptr)
            {
                m_ptr = strong.Get();
                m_control = static_cast<RefCounted*>(m_ptr)->Control();
                m_control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        WeakRefPtr(const WeakRefPtr& other) noexcept
            : m_ptr(other.m_ptr), m_control(other.m_control)
        {
            if (m_control != nullptr)
            {
                m_control->weak.fetch_add(1, std::memory_order_relaxed);
            }
        }

        WeakRefPtr(WeakRefPtr&& other) noexcept : m_ptr(other.m_ptr), m_control(other.m_control)
        {
            other.m_ptr = nullptr;
            other.m_control = nullptr;
        }

        ~WeakRefPtr()
        {
            if (m_control != nullptr)
            {
                detail::ReleaseWeak(m_control);
            }
        }

        WeakRefPtr& operator=(const WeakRefPtr& other) noexcept
        {
            if (this != &other)
            {
                if (other.m_control != nullptr)
                {
                    other.m_control->weak.fetch_add(1, std::memory_order_relaxed);
                }
                if (m_control != nullptr)
                {
                    detail::ReleaseWeak(m_control);
                }
                m_ptr = other.m_ptr;
                m_control = other.m_control;
            }
            return *this;
        }

        WeakRefPtr& operator=(WeakRefPtr&& other) noexcept
        {
            if (this != &other)
            {
                if (m_control != nullptr)
                {
                    detail::ReleaseWeak(m_control);
                }
                m_ptr = other.m_ptr;
                m_control = other.m_control;
                other.m_ptr = nullptr;
                other.m_control = nullptr;
            }
            return *this;
        }

        void Reset() noexcept
        {
            if (m_control != nullptr)
            {
                detail::ReleaseWeak(m_control);
            }
            m_ptr = nullptr;
            m_control = nullptr;
        }

        [[nodiscard]] bool Expired() const noexcept
        {
            return m_control == nullptr || m_control->strong.load(std::memory_order_acquire) == 0;
        }

        // Promotes to a strong RefPtr, or returns null if the object is gone.
        [[nodiscard]] RefPtr<T> Lock() const noexcept
        {
            if (m_control == nullptr)
            {
                return RefPtr<T>{};
            }

            u32 strong = m_control->strong.load(std::memory_order_relaxed);
            while (strong != 0)
            {
                if (m_control->strong.compare_exchange_weak(
                        strong, strong + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
                {
                    return RefPtr<T>{static_cast<T*>(m_ptr), AdoptRef{}};
                }
            }
            return RefPtr<T>{};
        }

    private:
        T* m_ptr = nullptr;
        detail::RefControl* m_control = nullptr;
    };
}
