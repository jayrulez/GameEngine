/// Draconic::Net - `draconic.net:wire` partition.
///
/// Bit-level wire serialization (docs/design/networking.md §3, `draconic.net.wire`): a BitWriter /
/// BitReader pair that packs values to the BIT rather than the byte, plus varints and ranged-float
/// quantization. This is the foundation the reliability layer, RPC, and replication delta all encode
/// through - bandwidth is the scarce resource, so the primitives are bit-exact and overflow-safe
/// (a reader that runs off the end reports !Ok() instead of reading garbage).
///
/// Bit order: LSB-first within a byte (value's low bit goes to the byte's bit 0). Writer and reader
/// are symmetric; the only contract is same call sequence on both sides.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Debug/Assert.h"

export module draconic.net:wire;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::net
{

    // Packs values bit-by-bit into a growing byte buffer (LSB-first). Call Data()/ByteCount() to read
    // the finished buffer (they flush any partial trailing byte, zero-padded).
    class BitWriter
    {
    public:
        // Write the low `bits` (0..32) bits of `value`.
        void WriteBits(u32 value, u32 bits)
        {
            DRACONIC_ASSERT(bits <= 32);
            if (bits == 0)
            {
                return;
            }
            const u32 masked = (bits >= 32) ? value : (value & ((1u << bits) - 1u));
            m_scratch |= (static_cast<u64>(masked) << m_scratchBits);
            m_scratchBits += bits;
            while (m_scratchBits >= 8)
            {
                m_bytes.PushBack(static_cast<byte>(m_scratch & 0xFFu));
                m_scratch >>= 8;
                m_scratchBits -= 8;
            }
        }

        void WriteBool(bool v) { WriteBits(v ? 1u : 0u, 1); }
        void WriteU8(u8 v) { WriteBits(v, 8); }
        void WriteU16(u16 v) { WriteBits(v, 16); }
        void WriteU32(u32 v) { WriteBits(v, 32); }
        void WriteU64(u64 v)
        {
            WriteBits(static_cast<u32>(v & 0xFFFFFFFFull), 32);
            WriteBits(static_cast<u32>(v >> 32), 32);
        }
        void WriteI32(i32 v) { WriteU32(static_cast<u32>(v)); }
        void WriteFloat(f32 v)
        {
            u32 bits = 0;
            MemCopy(&bits, &v, sizeof(bits));
            WriteU32(bits);
        }

        // Variable-length u32 (LEB128, 7 bits/byte): small values cost 1 byte. Ideal for lengths/ids.
        void WriteVarU32(u32 v)
        {
            while (v >= 0x80u)
            {
                WriteBits((v & 0x7Fu) | 0x80u, 8);
                v >>= 7;
            }
            WriteBits(v, 8);
        }

        // Quantize `v` into `bits` (1..32) over [min,max] (clamped). Lossy, bandwidth-cheap - the core
        // trick for positions/rotations. Symmetric with ReadFloatRanged(min,max,bits).
        void WriteFloatRanged(f32 v, f32 min, f32 max, u32 bits)
        {
            DRACONIC_ASSERT(bits >= 1 && bits <= 32);
            const f32 span = max - min;
            const f32 t = (span > 0.0f) ? Clamp((v - min) / span, 0.0f, 1.0f) : 0.0f;
            const u32 maxq = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            const u32 q = static_cast<u32>(t * static_cast<f32>(maxq) + 0.5f);
            WriteBits(q, bits);
        }

        void WriteBytes(Span<const byte> data)
        {
            for (byte b : data)
            {
                WriteBits(static_cast<u32>(static_cast<u8>(b)), 8);
            }
        }

        // The finished buffer (flushes a partial trailing byte, zero-padded). Idempotent.
        [[nodiscard]] Span<const byte> Data()
        {
            Flush();
            return Span<const byte>(m_bytes.Data(), m_bytes.Size());
        }
        [[nodiscard]] usize ByteCount()
        {
            Flush();
            return m_bytes.Size();
        }
        // Bits written so far (before flush padding).
        [[nodiscard]] usize BitCount() const noexcept
        {
            return m_bytes.Size() * 8u + m_scratchBits;
        }

        void Reset()
        {
            m_bytes.Clear();
            m_scratch = 0;
            m_scratchBits = 0;
        }

    private:
        void Flush()
        {
            if (m_scratchBits > 0)
            {
                m_bytes.PushBack(static_cast<byte>(m_scratch & 0xFFu));
                m_scratch = 0;
                m_scratchBits = 0;
            }
        }
        Array<byte> m_bytes;
        u64 m_scratch = 0; // pending bits not yet flushed to a byte (< 8 between calls)
        u32 m_scratchBits = 0;
    };

    // Reads back what BitWriter wrote. A read past the end sets an overflow flag (Ok() == false) and
    // returns zeros rather than reading garbage - so a truncated/malicious packet degrades safely.
    class BitReader
    {
    public:
        explicit BitReader(Span<const byte> data) noexcept : m_data(data) {}

        u32 ReadBits(u32 bits)
        {
            DRACONIC_ASSERT(bits <= 32);
            if (bits == 0)
            {
                return 0;
            }
            u32 result = 0, got = 0;
            while (got < bits)
            {
                if (m_scratchBits == 0)
                {
                    if (m_bytePos >= m_data.Size())
                    {
                        m_overflow = true;
                        return result;
                    }
                    m_scratch = static_cast<u64>(static_cast<u8>(m_data[m_bytePos++]));
                    m_scratchBits = 8;
                }
                const u32 take = Min(bits - got, m_scratchBits); // <= 8
                const u32 chunk = static_cast<u32>(m_scratch & ((1u << take) - 1u));
                result |= (chunk << got);
                m_scratch >>= take;
                m_scratchBits -= take;
                got += take;
            }
            return result;
        }

        bool ReadBool() { return ReadBits(1) != 0; }
        u8 ReadU8() { return static_cast<u8>(ReadBits(8)); }
        u16 ReadU16() { return static_cast<u16>(ReadBits(16)); }
        u32 ReadU32() { return ReadBits(32); }
        u64 ReadU64()
        {
            const u64 lo = ReadBits(32);
            return lo | (static_cast<u64>(ReadBits(32)) << 32);
        }
        i32 ReadI32() { return static_cast<i32>(ReadU32()); }
        f32 ReadFloat()
        {
            const u32 bits = ReadU32();
            f32 v = 0.0f;
            MemCopy(&v, &bits, sizeof(v));
            return v;
        }

        u32 ReadVarU32()
        {
            u32 v = 0, shift = 0;
            for (;;)
            {
                const u32 b = ReadBits(8);
                v |= (b & 0x7Fu) << shift;
                if ((b & 0x80u) == 0 || shift >= 28)
                {
                    break;
                }
                shift += 7;
            }
            return v;
        }

        f32 ReadFloatRanged(f32 min, f32 max, u32 bits)
        {
            DRACONIC_ASSERT(bits >= 1 && bits <= 32);
            const u32 maxq = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
            const u32 q = ReadBits(bits);
            const f32 t = (maxq > 0) ? static_cast<f32>(q) / static_cast<f32>(maxq) : 0.0f;
            return min + t * (max - min);
        }

        void ReadBytes(Span<byte> out)
        {
            for (byte& b : out)
            {
                b = static_cast<byte>(ReadU8());
            }
        }

        // False once any read ran past the end of the buffer (truncated/corrupt packet).
        [[nodiscard]] bool Ok() const noexcept { return !m_overflow; }
        // No more bits to read (and nothing overflowed).
        [[nodiscard]] bool AtEnd() const noexcept
        {
            return m_bytePos >= m_data.Size() && m_scratchBits == 0;
        }

    private:
        Span<const byte> m_data;
        usize m_bytePos = 0;
        u64 m_scratch = 0;
        u32 m_scratchBits = 0;
        bool m_overflow = false;
    };

}
