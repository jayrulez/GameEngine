/// Draconic::Geometry - the `:index_buffer` partition.
///
/// A format-tagged index buffer (16- or 32-bit), stored as raw bytes so it uploads
/// straight to a GPU index buffer. An internal write cursor supports streaming
/// append (Add/AddTriangle) for the primitive generators + importers.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.geometry:index_buffer;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::geometry
{

    class IndexBuffer
    {
    public:
        enum class Format : u8
        {
            U16,
            U32
        };

        explicit IndexBuffer(Format format = Format::U32) : m_format(format) {}

        [[nodiscard]] Format GetFormat() const noexcept { return m_format; }
        [[nodiscard]] u32 IndexSize() const noexcept { return m_format == Format::U16 ? 2u : 4u; }
        [[nodiscard]] u32 Count() const noexcept { return m_count; }
        [[nodiscard]] u32 DataSize() const noexcept { return m_count * IndexSize(); }
        [[nodiscard]] const u8* RawData() const noexcept
        {
            return m_count > 0 ? m_data.Data() : nullptr;
        }

        void Reserve(u32 count)
        {
            const u32 needed = count * IndexSize();
            if (m_data.Size() < needed)
            {
                m_data.Resize(needed);
            }
        }

        // Sets the logical count (growing storage) and rewinds the append cursor.
        void Resize(u32 count)
        {
            m_count = count;
            Reserve(count);
            m_writePos = 0;
        }

        void Clear()
        {
            m_count = 0;
            m_writePos = 0;
        }

        void Set(u32 index, u32 value)
        {
            if (index >= m_count)
            {
                return;
            }
            const u32 offset = index * IndexSize();
            if (m_format == Format::U16)
            {
                const u16 v = static_cast<u16>(value);
                MemCopy(m_data.Data() + offset, &v, 2);
            }
            else
            {
                MemCopy(m_data.Data() + offset, &value, 4);
            }
        }

        [[nodiscard]] u32 Get(u32 index) const
        {
            if (index >= m_count)
            {
                return 0;
            }
            const u32 offset = index * IndexSize();
            if (m_format == Format::U16)
            {
                u16 v = 0;
                MemCopy(&v, m_data.Data() + offset, 2);
                return v;
            }
            u32 v = 0;
            MemCopy(&v, m_data.Data() + offset, 4);
            return v;
        }

        // Append using the internal cursor (call Resize/Reserve first).
        void Add(u32 value)
        {
            if (m_writePos >= m_count)
            {
                return;
            }
            Set(m_writePos, value);
            ++m_writePos;
        }
        void AddTriangle(u32 a, u32 b, u32 c)
        {
            Add(a);
            Add(b);
            Add(c);
        }

    private:
        Array<u8> m_data;
        u32 m_count = 0;
        u32 m_writePos = 0;
        Format m_format = Format::U32;
    };

} // namespace draconic::geometry
