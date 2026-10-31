// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include <cstring>
#include <type_traits>

export module foundation.core:array;

import :base;
import :allocator;
import :span;

export namespace foundation::core
{
    // =======================================================================
    // Array - growable, allocator-backed dynamic array.
    // =======================================================================
    template <typename T>
    class Array
    {
    public:
        Array() noexcept : m_allocator(&DefaultAllocator()) {}
        explicit Array(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        // The allocator backing this array. Owners thread it into work done on the array's
        // behalf (scratch, workers) so those allocations come from the same place.
        [[nodiscard]] IAllocator& Allocator() const noexcept { return *m_allocator; }

        // Sized construction: `count` value-initialized elements.
        explicit Array(usize count, IAllocator& allocator = DefaultAllocator())
            : m_allocator(&allocator)
        {
            Resize(count);
        }

        // Sized construction: `count` elements copy-initialized from `value`.
        Array(usize count, const T& value, IAllocator& allocator = DefaultAllocator())
            : m_allocator(&allocator)
        {
            Resize(count, value);
        }

        Array(const Array& other) : m_allocator(other.m_allocator)
        {
            Reserve(other.m_size);
            for (usize i = 0; i < other.m_size; ++i)
            {
                Construct<T>(&m_data[i], other.m_data[i]);
            }
            m_size = other.m_size;
        }

        Array(Array&& other) noexcept
            : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity),
              m_allocator(other.m_allocator)
        {
            other.m_data = nullptr;
            other.m_size = 0;
            other.m_capacity = 0;
        }

        Array& operator=(const Array& other)
        {
            if (this != &other)
            {
                Clear();
                Reserve(other.m_size);
                for (usize i = 0; i < other.m_size; ++i)
                {
                    Construct<T>(&m_data[i], other.m_data[i]);
                }
                m_size = other.m_size;
            }
            return *this;
        }

        Array& operator=(Array&& other) noexcept
        {
            if (this != &other)
            {
                Destroy();
                m_data = other.m_data;
                m_size = other.m_size;
                m_capacity = other.m_capacity;
                m_allocator = other.m_allocator;
                other.m_data = nullptr;
                other.m_size = 0;
                other.m_capacity = 0;
            }
            return *this;
        }

        ~Array() { Destroy(); }

        // --- capacity ------------------------------------------------------
        [[nodiscard]] usize Size() const noexcept { return m_size; }
        [[nodiscard]] usize Capacity() const noexcept { return m_capacity; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }

        void Reserve(usize newCapacity)
        {
            if (newCapacity <= m_capacity)
            {
                return;
            }

            T* newData =
                static_cast<T*>(m_allocator->Allocate(newCapacity * sizeof(T), alignof(T)));
            DIAGNOSTIC_ASSERT_MSG(newData != nullptr, "Array allocation failed");

            // A trivially copyable T relocates as one block. A per-element move and destruct
            // loop on every growth of a byte buffer is slow at -O0 (a large texture's mip chain,
            // reallocated per level, costs seconds of its cook).
            if constexpr (std::is_trivially_copyable_v<T>)
            {
                if (m_size > 0)
                {
                    std::memcpy(static_cast<void*>(newData), static_cast<const void*>(m_data),
                                m_size * sizeof(T));
                }
            }
            else
            {
                for (usize i = 0; i < m_size; ++i)
                {
                    Construct<T>(&newData[i], Move(m_data[i]));
                    Destruct(&m_data[i]);
                }
            }

            if (m_data != nullptr)
            {
                m_allocator->Free(m_data);
            }
            m_data = newData;
            m_capacity = newCapacity;
        }

        void Resize(usize newSize)
        {
            if (newSize < m_size)
            {
                for (usize i = newSize; i < m_size; ++i)
                {
                    Destruct(&m_data[i]);
                }
            }
            else if (newSize > m_size)
            {
                ReserveForGrowth(newSize);
                // Value-initialization of a trivial T is a zero fill: the same bytes the
                // per-element loop writes, without an -O0 call per element.
                if constexpr (std::is_trivially_default_constructible_v<T> &&
                              std::is_trivially_copyable_v<T>)
                {
                    std::memset(static_cast<void*>(m_data + m_size), 0,
                                (newSize - m_size) * sizeof(T));
                }
                else
                {
                    for (usize i = m_size; i < newSize; ++i)
                    {
                        Construct<T>(&m_data[i]);
                    }
                }
            }
            m_size = newSize;
        }

        // Resize, copy-initializing any new elements from `value`.
        void Resize(usize newSize, const T& value)
        {
            if (newSize < m_size)
            {
                for (usize i = newSize; i < m_size; ++i)
                {
                    Destruct(&m_data[i]);
                }
            }
            else if (newSize > m_size)
            {
                ReserveForGrowth(newSize);
                for (usize i = m_size; i < newSize; ++i)
                {
                    Construct<T>(&m_data[i], value);
                }
            }
            m_size = newSize;
        }

        // Destroys all elements; keeps the allocated capacity.
        void Clear() noexcept
        {
            for (usize i = 0; i < m_size; ++i)
            {
                Destruct(&m_data[i]);
            }
            m_size = 0;
        }

        // --- element access ------------------------------------------------
        [[nodiscard]] T& operator[](usize index) noexcept
        {
            DIAGNOSTIC_ASSERT(index < m_size);
            return m_data[index];
        }
        [[nodiscard]] const T& operator[](usize index) const noexcept
        {
            DIAGNOSTIC_ASSERT(index < m_size);
            return m_data[index];
        }

        [[nodiscard]] T& Front() noexcept
        {
            DIAGNOSTIC_ASSERT(m_size > 0);
            return m_data[0];
        }
        [[nodiscard]] const T& Front() const noexcept
        {
            DIAGNOSTIC_ASSERT(m_size > 0);
            return m_data[0];
        }
        [[nodiscard]] T& Back() noexcept
        {
            DIAGNOSTIC_ASSERT(m_size > 0);
            return m_data[m_size - 1];
        }
        [[nodiscard]] const T& Back() const noexcept
        {
            DIAGNOSTIC_ASSERT(m_size > 0);
            return m_data[m_size - 1];
        }

        [[nodiscard]] T* Data() noexcept { return m_data; }
        [[nodiscard]] const T* Data() const noexcept { return m_data; }

        // --- modifiers -----------------------------------------------------
        T& PushBack(const T& value)
        {
            EnsureCapacityForOne();
            Construct<T>(&m_data[m_size], value);
            return m_data[m_size++];
        }

        T& PushBack(T&& value)
        {
            EnsureCapacityForOne();
            Construct<T>(&m_data[m_size], Move(value));
            return m_data[m_size++];
        }

        template <typename... Args>
        T& EmplaceBack(Args&&... args)
        {
            EnsureCapacityForOne();
            Construct<T>(&m_data[m_size], Forward<Args>(args)...);
            return m_data[m_size++];
        }

        void PopBack() noexcept
        {
            DIAGNOSTIC_ASSERT(m_size > 0);
            Destruct(&m_data[--m_size]);
        }

        // Removes element `index`, shifting the tail down (order preserved).
        void RemoveAt(usize index)
        {
            DIAGNOSTIC_ASSERT(index < m_size);
            for (usize i = index; i + 1 < m_size; ++i)
            {
                m_data[i] = Move(m_data[i + 1]);
            }
            Destruct(&m_data[--m_size]);
        }

        // Removes element `index` by swapping in the last element (O(1), order not preserved).
        void RemoveAtSwap(usize index)
        {
            DIAGNOSTIC_ASSERT(index < m_size);
            if (index != m_size - 1)
            {
                m_data[index] = Move(m_data[m_size - 1]);
            }
            Destruct(&m_data[--m_size]);
        }

        // Inserts `value` before `index` (index == Size appends), shifting the tail up. Order preserved.
        T& Insert(usize index, const T& value)
        {
            DIAGNOSTIC_ASSERT(index <= m_size);
            EnsureCapacityForOne();
            if (index == m_size)
            {
                Construct<T>(&m_data[m_size], value);
                return m_data[m_size++];
            }
            Construct<T>(&m_data[m_size], Move(m_data[m_size - 1]));
            for (usize i = m_size - 1; i > index; --i)
            {
                m_data[i] = Move(m_data[i - 1]);
            }
            m_data[index] = value;
            ++m_size;
            return m_data[index];
        }

        T& Insert(usize index, T&& value)
        {
            DIAGNOSTIC_ASSERT(index <= m_size);
            EnsureCapacityForOne();
            if (index == m_size)
            {
                Construct<T>(&m_data[m_size], Move(value));
                return m_data[m_size++];
            }
            Construct<T>(&m_data[m_size], Move(m_data[m_size - 1]));
            for (usize i = m_size - 1; i > index; --i)
            {
                m_data[i] = Move(m_data[i - 1]);
            }
            m_data[index] = Move(value);
            ++m_size;
            return m_data[index];
        }

        // Stable in-place sort by `less(a, b)` (returns true if a should come before b). Insertion
        // sort, fine for the small collections this is used on (focus lists, tracks, ...).
        template <typename Compare>
        void Sort(Compare less)
        {
            for (usize i = 1; i < m_size; ++i)
            {
                T key = Move(m_data[i]);
                usize j = i;
                while (j > 0 && less(key, m_data[j - 1]))
                {
                    m_data[j] = Move(m_data[j - 1]);
                    --j;
                }
                m_data[j] = Move(key);
            }
        }

        // --- views / iteration ---------------------------------------------
        [[nodiscard]] Span<T> AsSpan() noexcept { return Span<T>{m_data, m_size}; }
        [[nodiscard]] Span<const T> AsSpan() const noexcept
        {
            return Span<const T>{m_data, m_size};
        }

        [[nodiscard]] T* begin() noexcept { return m_data; }
        [[nodiscard]] T* end() noexcept { return m_data + m_size; }
        [[nodiscard]] const T* begin() const noexcept { return m_data; }
        [[nodiscard]] const T* end() const noexcept { return m_data + m_size; }

    private:
        void EnsureCapacityForOne()
        {
            if (m_size == m_capacity)
            {
                Reserve(m_capacity == 0 ? kInitialCapacity : m_capacity * 2);
            }
        }

        // A Resize that grows an array already holding data reserves geometrically (1.5x),
        // like PushBack: a buffer built up step by step (a mip chain) reallocates a few times,
        // not once per step. A first Resize from empty stays exact: it is usually the final
        // size (a decoded image), and doubling it would waste half the buffer.
        void ReserveForGrowth(usize needed)
        {
            if (needed <= m_capacity)
            {
                return;
            }
            const usize grown = (m_capacity > 0) ? m_capacity + m_capacity / 2 : 0;
            Reserve(needed > grown ? needed : grown);
        }

        void Destroy() noexcept
        {
            Clear();
            if (m_data != nullptr)
            {
                m_allocator->Free(m_data);
                m_data = nullptr;
            }
            m_capacity = 0;
        }

        static constexpr usize kInitialCapacity = 8;

        T* m_data = nullptr;
        usize m_size = 0;
        usize m_capacity = 0;
        IAllocator* m_allocator = nullptr;
    };
}
