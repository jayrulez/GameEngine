// Draconic Foundation - :binary_serializer partition
//
// Raw binary backend (as-stored bytes) over an IStream, built on BinaryReader /
// BinaryWriter. Names and object scopes carry no information here, so they are
// dropped; arrays and strings store a u32 count/length prefix, and scalars
// store their natural width.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:binary_serializer;

import :base;
import :serializer;
import :string;
import :guid;
import :io;
import :binary_io;
import :unique_ptr;
import :allocator;
import :function;
import :array; // frame stack + RawRemainder blob
import :span; // MemoryStream::Bytes()

export namespace draconic::foundation
{
    namespace detail
    {
        // An IStream that forwards to a SWAPPABLE target. BinarySerializer's reader/writer wrap one of
        // these so a framed region can redirect all I/O into a sub-stream without rebinding them.
        class RedirectStream final : public IStream
        {
        public:
            IStream* target = nullptr;

            [[nodiscard]] u64 Read(void* d, u64 n) override { return target ? target->Read(d, n) : 0; }
            [[nodiscard]] u64 Write(const void* d, u64 n) override
            {
                return target ? target->Write(d, n) : 0;
            }
            [[nodiscard]] i64 Seek(i64 o, SeekOrigin origin) override
            {
                return target ? target->Seek(o, origin) : -1;
            }
            [[nodiscard]] i64 Tell() const override { return target ? target->Tell() : -1; }
            [[nodiscard]] i64 Size() const override { return target ? target->Size() : -1; }
            [[nodiscard]] bool IsValid() const override
            {
                return target != nullptr && target->IsValid();
            }
        };
    }

    // Backend: raw little-endian-as-stored binary over an IStream.
    class BinarySerializer final : public Serializer
    {
    public:
        BinarySerializer(IStream& stream, SerializeMode mode) noexcept
            : Serializer(mode), m_reader(m_redirect), m_writer(m_redirect)
        {
            m_redirect.target = &stream;
        }

        // Naming and object scopes are inherited as no-ops from Serializer; a
        // flat byte stream carries no names. Arrays are length-prefixed.
        void BeginArray(u32& count) override { Scalar(&count, ScalarKind::UInt32); }

        void Scalar(void* value, ScalarKind kind) override { RawBytes(value, ScalarSize(kind)); }

        void Text(String& value) override
        {
            if (IsWriting())
            {
                if (!m_writer.WriteString(value))
                {
                    Fail(ErrorCode::Internal);
                }
            }
            else
            {
                if (!m_reader.ReadString(value))
                {
                    Fail(ErrorCode::Internal);
                }
            }
        }

        void Blob(void* data, usize size) override { RawBytes(data, size); }

        // Compact: the raw 16 bytes (two u64 halves), not the canonical string. (Traktor-style.)
        void GuidValue(Guid& value) override { RawBytes(&value, sizeof(Guid)); }

        // === Framed regions (unknown-section passthrough) ===
        // WRITE: redirect the enclosed writes into a fresh sub-buffer; End emits it as u32-length +
        // bytes. READ: read the u32 length, pull that many bytes into a bounded sub-buffer, read from
        // it; End restores the outer stream, discarding any bytes the caller did not consume (skip).
        void BeginFramedRegion() override
        {
            UniquePtr<MemoryStream> buffer = MakeUnique<MemoryStream>(DefaultAllocator());
            if (IsReading())
            {
                u32 length = 0;
                if (!m_reader.Read(length))
                {
                    Fail(ErrorCode::Internal);
                    length = 0;
                }
                if (length > 0)
                {
                    Array<u8> tmp;
                    tmp.Resize(length);
                    if (!m_reader.ReadBytes(tmp.Data(), length))
                    {
                        Fail(ErrorCode::Internal);
                    }
                    (void)buffer->Write(tmp.Data(), length);
                    (void)buffer->Seek(0, SeekOrigin::Begin);
                }
            }
            IStream* const previous = m_redirect.target;
            m_redirect.target = buffer.Get();
            m_frames.PushBack(Frame{previous, Move(buffer)});
        }

        void EndFramedRegion() override
        {
            if (m_frames.IsEmpty())
            {
                return;
            }
            Frame frame = Move(m_frames[m_frames.Size() - 1]);
            m_frames.PopBack();
            m_redirect.target = frame.previous; // restore BEFORE emitting, so writes hit the outer stream
            if (IsWriting())
            {
                const Span<const byte> bytes = frame.buffer->Bytes();
                const u32 length = static_cast<u32>(bytes.Size());
                if (!m_writer.Write(length) ||
                    (length > 0 && !m_writer.WriteBytes(bytes.Data(), length)))
                {
                    Fail(ErrorCode::Internal);
                }
            }
        }

        bool RawRemainder(Array<u8>& blob) override
        {
            if (m_frames.IsEmpty())
            {
                return false; // binary needs an active frame to know where the region ends
            }
            MemoryStream* buffer = m_frames[m_frames.Size() - 1].buffer.Get();
            if (IsWriting())
            {
                if (!blob.IsEmpty() && !m_writer.WriteBytes(blob.Data(), blob.Size()))
                {
                    Fail(ErrorCode::Internal);
                    return false;
                }
                return true;
            }
            const i64 size = buffer->Size();
            const i64 pos = buffer->Tell();
            const usize remaining = (size > pos) ? static_cast<usize>(size - pos) : 0;
            blob.Clear();
            blob.Resize(remaining);
            if (remaining > 0 && !m_reader.ReadBytes(blob.Data(), remaining))
            {
                Fail(ErrorCode::Internal);
                return false;
            }
            return true;
        }

    private:
        // Moves `size` bytes in whichever direction this serializer runs.
        void RawBytes(void* data, usize size)
        {
            if (size == 0)
            {
                return;
            }

            if (IsWriting())
            {
                if (!m_writer.WriteBytes(data, size))
                {
                    Fail(ErrorCode::Internal);
                }
            }
            else
            {
                if (!m_reader.ReadBytes(data, size))
                {
                    Fail(ErrorCode::Internal);
                }
            }
        }

        static usize ScalarSize(ScalarKind kind) noexcept
        {
            switch (kind)
            {
            case ScalarKind::Bool:
                return sizeof(bool);
            case ScalarKind::Int8:
            case ScalarKind::UInt8:
                return 1;
            case ScalarKind::Int16:
            case ScalarKind::UInt16:
                return 2;
            case ScalarKind::Int32:
            case ScalarKind::UInt32:
            case ScalarKind::Float32:
                return 4;
            case ScalarKind::Int64:
            case ScalarKind::UInt64:
            case ScalarKind::Float64:
                return 8;
            }
            return 0;
        }

        // A framed region in flight: the sub-buffer the enclosed I/O uses, plus the target to restore
        // on End. Declared BEFORE the reader/writer so m_redirect is live when they bind to it.
        struct Frame
        {
            IStream* previous = nullptr;
            UniquePtr<MemoryStream> buffer;
        };

        detail::RedirectStream m_redirect; // reader/writer bind here; target swaps for framing
        BinaryReader m_reader;
        BinaryWriter m_writer;
        Array<Frame> m_frames; // nested framed regions (stack)
    };

    // Built-in SerializerFactory for binary serialization.
    namespace detail
    {
        struct BinarySerializerContext final : SerializerContext
        {
            BinarySerializer impl;
            BinarySerializerContext(IStream& stream, SerializeMode mode) : impl(stream, mode)
            {
                serializer = &impl;
            }
        };
    }

    [[nodiscard]] inline SerializerFactory BinarySerializerFactory()
    {
        return SerializerFactory{
            [](IStream& stream, SerializeMode mode) -> UniquePtr<SerializerContext>
            {
                return MakeUnique<detail::BinarySerializerContext>(DefaultAllocator(), stream,
                                                                   mode);
            }};
    }
}
