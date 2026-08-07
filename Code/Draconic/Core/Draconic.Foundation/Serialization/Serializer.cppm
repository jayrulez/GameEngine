// Draconic Foundation - :serializer partition
//
// Serializer: the concrete base over ISerializer that holds the mode, version,
// and a sticky error Status. Backends (e.g. BinarySerializer) extend this.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:serializer;

export import :iserializer;
import :base;
import :string;
import :guid;
import :unique_ptr;
import :allocator;
import :function;
import :io;
import :array; // data-version scope stack

export namespace draconic::foundation
{
    // Backends extend this, not ISerializer directly.
    class Serializer : public ISerializer
    {
    public:
        explicit Serializer(SerializeMode mode) noexcept : m_mode(mode) {}

        [[nodiscard]] SerializeMode Mode() const noexcept override { return m_mode; }

        // Data-version scopes: a flat stack of chains ([start,end) per scope). The concrete
        // type is always entry 0 of its scope.
        [[nodiscard]] u32 Version() const noexcept override
        {
            if (m_scopeStarts.IsEmpty())
            {
                return 0;
            }
            const usize start = m_scopeStarts[m_scopeStarts.Size() - 1];
            return (start < m_versionStack.Size()) ? m_versionStack[start].version : 0;
        }
        [[nodiscard]] u32 Version(u64 typeId) const noexcept override
        {
            if (m_scopeStarts.IsEmpty())
            {
                return 0;
            }
            const usize start = m_scopeStarts[m_scopeStarts.Size() - 1];
            for (usize i = start; i < m_versionStack.Size(); ++i)
            {
                if (m_versionStack[i].typeId == typeId)
                {
                    return m_versionStack[i].version;
                }
            }
            return 0;
        }
        void PushVersionScope(const SerializedDataVersion* chain, usize count) override
        {
            m_scopeStarts.PushBack(m_versionStack.Size());
            for (usize i = 0; i < count; ++i)
            {
                m_versionStack.PushBack(chain[i]);
            }
        }
        void PopVersionScope() override
        {
            if (m_scopeStarts.IsEmpty())
            {
                return;
            }
            const usize start = m_scopeStarts[m_scopeStarts.Size() - 1];
            m_scopeStarts.PopBack();
            while (m_versionStack.Size() > start)
            {
                m_versionStack.PopBack();
            }
        }

        [[nodiscard]] Status GetStatus() const noexcept { return m_status; }
        [[nodiscard]] bool IsOk() const noexcept { return m_status.IsOk(); }

        [[nodiscard]] bool IsReading() const noexcept { return m_mode == SerializeMode::Read; }
        [[nodiscard]] bool IsWriting() const noexcept { return m_mode == SerializeMode::Write; }

        // True for backends whose structure is discoverable from the data (keyed/text - XML, JSON),
        // false for POSITIONAL backends (binary) that need explicit framing/versioning. A store can
        // use this to add a format-version field only where the layout is positional and not
        // otherwise self-describing (so a self-describing file stays readable across format changes).
        [[nodiscard]] virtual bool IsSelfDescribing() const noexcept { return false; }

        // Naming and object/array scopes carry no information in unkeyed formats;
        // default them to no-ops. Keyed/text backends override what they need.
        // (BeginArray/Scalar/Text/Blob stay pure - every backend must move data.)
        void Key(const char* name) noexcept override { (void)name; }
        void BeginObject() override {}
        void EndObject() override {}
        void EndArray() override {}

        // === Framed regions (unknown-section passthrough) ===
        // Bracket a SELF-DELIMITING sub-region so a payload whose type the running build cannot
        // instantiate can be captured or SKIPPED uniformly across backends. Binary length-prefixes the
        // enclosed bytes (a sub-stream + u32 length - the Scene blob pattern); self-describing formats
        // (XML) rely on element boundaries and no-op. Default = no-op, so positional backends that never
        // frame keep today's behavior; a store that wants skippable sections (Settings) frames EVERY
        // section between these. Regions may nest.
        virtual void BeginFramedRegion() {}
        virtual void EndFramedRegion() {}

        // Capture (READ) / re-inject (WRITE) the CURRENT framed region's remaining content as raw bytes,
        // for a section whose type the build cannot instantiate. READ consumes the rest of the region
        // into `blob`; WRITE emits `blob` back verbatim so a later build that DOES know the type reads
        // it unchanged. XML moves the remaining child subtree; binary moves the framed bytes. Returns
        // false where an unknown region cannot be preserved (a positional backend with no active frame),
        // so the caller can drop it with a warning. Default: unsupported.
        virtual bool RawRemainder(Array<u8>& blob)
        {
            (void)blob;
            return false;
        }

        // Default Guid representation: the canonical 36-char string (readable in text backends).
        // BinarySerializer overrides this with the compact raw-16-bytes form.
        void GuidValue(Guid& value) override
        {
            String text;
            if (m_mode == SerializeMode::Write)
            {
                utf8char buffer[37];
                value.ToChars(buffer);
                text = String{StringView{buffer, 36}};
            }
            Text(text);
            if (m_mode == SerializeMode::Read)
            {
                Guid parsed{};
                if (Guid::TryParse(text.AsView(), parsed))
                {
                    value = parsed;
                }
            }
        }

    protected:
        void Fail(ErrorCode code) noexcept
        {
            if (m_status.IsOk())
            {
                m_status = code;
            }
        }

        SerializeMode m_mode;
        Array<SerializedDataVersion> m_versionStack; // flat entries of all open scopes
        Array<usize> m_scopeStarts;                  // per-scope start index into the stack
        Status m_status{};
    };

    // =======================================================================
    // SerializerContext — owns a Serializer and any intermediate state
    // (e.g., an XmlDocument) that must outlive it. The factory returns one
    // of these; the caller uses `serializer` and then destroys the context.
    // =======================================================================
    struct SerializerContext
    {
        virtual ~SerializerContext() = default;

        // The serializer to use. Owned by this context (destroyed when the
        // context is destroyed).
        Serializer* serializer = nullptr;

        // Called after write-mode serialization is complete. Implementations
        // flush the serialized data to the stream (e.g., XML text output).
        // Binary writes directly to the stream, so the default is a no-op.
        virtual void Flush(IStream& /*out*/) {}
    };

    // Creates a SerializerContext for a given stream and mode.
    using SerializerFactory = Function<UniquePtr<SerializerContext>(IStream&, SerializeMode)>;
}
