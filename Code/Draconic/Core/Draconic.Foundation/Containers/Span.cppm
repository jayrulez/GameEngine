module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"
#include <type_traits>

export module draconic.foundation:span;

import :base;

export namespace draconic::foundation
{
    // =======================================================================
    // Span - non-owning view over contiguous elements.
    // =======================================================================
    template <typename T>
    class Span
    {
    public:
        Span() noexcept = default;
        Span(T* data, usize size) noexcept : m_data(data), m_size(size) {}

        template <usize N>
        Span(T (&array)[N]) noexcept : m_data(array), m_size(N)
        {
        }

        // Qualification conversion, e.g. Span<byte> -> Span<const byte>: construct from a Span<U>
        // whose element pointer converts to T* under array-form rules (adds const; excludes
        // derived->base slicing and unrelated types). Lets a mutable buffer bind where a read-only
        // view is expected.
        template <typename U>
            requires(!std::is_same_v<U, T> && std::is_convertible_v<U (*)[], T (*)[]>)
        Span(const Span<U>& other) noexcept : m_data(other.Data()), m_size(other.Size())
        {
        }

        [[nodiscard]] T* Data() const noexcept { return m_data; }
        [[nodiscard]] usize Size() const noexcept { return m_size; }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_size == 0; }

        [[nodiscard]] T& operator[](usize index) const noexcept
        {
            DRACONIC_ASSERT(index < m_size);
            return m_data[index];
        }

        [[nodiscard]] T& Front() const noexcept
        {
            DRACONIC_ASSERT(m_size > 0);
            return m_data[0];
        }
        [[nodiscard]] T& Back() const noexcept
        {
            DRACONIC_ASSERT(m_size > 0);
            return m_data[m_size - 1];
        }

        [[nodiscard]] Span SubSpan(usize offset, usize count) const noexcept
        {
            DRACONIC_ASSERT(offset + count <= m_size);
            return Span{m_data + offset, count};
        }

        [[nodiscard]] T* begin() const noexcept { return m_data; }
        [[nodiscard]] T* end() const noexcept { return m_data + m_size; }

    private:
        T* m_data = nullptr;
        usize m_size = 0;
    };
}
