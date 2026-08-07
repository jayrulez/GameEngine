module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.foundation:ring_buffer;

import :base;
import :allocator;

export namespace draconic::foundation
{
    // =======================================================================
    // RingBuffer - fixed-capacity circular FIFO. PushBack/PopFront; full pushes
    // are rejected (returns false). Move-only.
    // =======================================================================
    template <typename T>
    class RingBuffer
    {
    public:
        RingBuffer() noexcept : m_allocator(&DefaultAllocator()) {}

        explicit RingBuffer(usize capacity, IAllocator& allocator = DefaultAllocator())
            : m_allocator(&allocator)
        {
            if (capacity > 0)
            {
                m_data = static_cast<T*>(m_allocator->Allocate(capacity * sizeof(T), alignof(T)));
                DRACONIC_ASSERT_MSG(m_data != nullptr, "RingBuffer allocation failed");
                m_capacity = capacity;
            }
        }

        RingBuffer(RingBuffer&& other) noexcept
            : m_data(other.m_data), m_capacity(other.m_capacity), m_head(other.m_head),
              m_count(other.m_count), m_allocator(other.m_allocator)
        {
            other.m_data = nullptr;
            other.m_capacity = 0;
            other.m_head = 0;
            other.m_count = 0;
        }

        RingBuffer& operator=(RingBuffer&& other) noexcept
        {
            if (this != &other)
            {
                Destroy();
                m_data = other.m_data;
                m_capacity = other.m_capacity;
                m_head = other.m_head;
                m_count = other.m_count;
                m_allocator = other.m_allocator;
                other.m_data = nullptr;
                other.m_capacity = 0;
                other.m_head = 0;
                other.m_count = 0;
            }
            return *this;
        }

        RingBuffer(const RingBuffer&) = delete;
        RingBuffer& operator=(const RingBuffer&) = delete;

        ~RingBuffer() { Destroy(); }

        [[nodiscard]] usize Size() const noexcept { return m_count; }
        [[nodiscard]] usize Capacity() const noexcept { return m_capacity; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_count == 0; }
        [[nodiscard]] bool IsFull() const noexcept { return m_count == m_capacity; }

        bool PushBack(const T& value)
        {
            if (IsFull())
            {
                return false;
            }
            Construct<T>(&m_data[TailIndex()], value);
            ++m_count;
            return true;
        }

        bool PushBack(T&& value)
        {
            if (IsFull())
            {
                return false;
            }
            Construct<T>(&m_data[TailIndex()], Move(value));
            ++m_count;
            return true;
        }

        // Moves the front element into `out` and removes it; false if empty.
        bool PopFront(T& out)
        {
            if (IsEmpty())
            {
                return false;
            }
            out = Move(m_data[m_head]);
            Destruct(&m_data[m_head]);
            m_head = (m_head + 1) % m_capacity;
            --m_count;
            return true;
        }

        [[nodiscard]] T& Front() noexcept
        {
            DRACONIC_ASSERT(m_count > 0);
            return m_data[m_head];
        }
        [[nodiscard]] T& Back() noexcept
        {
            DRACONIC_ASSERT(m_count > 0);
            return m_data[(m_head + m_count - 1) % m_capacity];
        }

        // Indexed from the front (0 == oldest).
        [[nodiscard]] T& operator[](usize index) noexcept
        {
            DRACONIC_ASSERT(index < m_count);
            return m_data[(m_head + index) % m_capacity];
        }
        [[nodiscard]] const T& operator[](usize index) const noexcept
        {
            DRACONIC_ASSERT(index < m_count);
            return m_data[(m_head + index) % m_capacity];
        }

        void Clear() noexcept
        {
            for (usize i = 0; i < m_count; ++i)
            {
                Destruct(&m_data[(m_head + i) % m_capacity]);
            }
            m_head = 0;
            m_count = 0;
        }

    private:
        [[nodiscard]] usize TailIndex() const noexcept { return (m_head + m_count) % m_capacity; }

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

        T* m_data = nullptr;
        usize m_capacity = 0;
        usize m_head = 0;
        usize m_count = 0;
        IAllocator* m_allocator = nullptr;
    };
}
