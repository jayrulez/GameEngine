// Cooked shader pack - the dist's compiler-free shader store.
//
// A shipped dist carries no DXC/naga: every (shader name, stage, variant, backend format) the
// runtime can request is precompiled into this pack at export time (see the cook, D3) and looked up
// by the dist provider (D4). The blob is backend bytecode/text: SPIR-V (Vulkan), DXIL (DX12), or
// WGSL text (WebGPU). Binary, versioned, self-describing (carries the name table so the provider can
// still enumerate the built-ins for tooling).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.shaders:pack;

import draconic.foundation;
import :types;
import :flags;

using namespace draconic::foundation;

export namespace draconic::shaders
{
    // The cooked blob format for a target backend. WGSL is TEXT; the other two are bytecode.
    enum class CookedShaderFormat : u32
    {
        SpirV = 0, // Vulkan (+ native wgpu-native accepts SPIR-V too)
        Dxil = 1,  // DX12
        Wgsl = 2,  // WebGPU (browser)
    };

    // A compiler-free store of cooked shader variants, addressable by (name, stage, flags, format).
    // Filled by the export cook; read by the dist shader-source provider. One pack can hold multiple
    // formats (a desktop dist may ship SPIR-V + DXIL); export stages only the target backend's.
    class CookedShaderPack
    {
    public:
        // Add one cooked variant. `name` is the ShaderSystem name (path stem). Copies the blob.
        void Add(StringView name, ShaderStage stage, ShaderFlags flags, CookedShaderFormat format,
                 Span<const byte> blob)
        {
            const u64 nameHash = ShaderNameHash(name);
            if (m_names.Find(nameHash) == nullptr)
            {
                m_names.InsertOrAssign(nameHash, String(name));
            }
            Entry e;
            e.nameHash = nameHash;
            e.stage = stage;
            e.flags = flags;
            e.format = format;
            e.blob.Resize(blob.Size());
            if (!blob.IsEmpty())
            {
                MemCopy(e.blob.Data(), blob.Data(), blob.Size());
            }
            const u64 key = CombineKey(nameHash, stage, flags, format);
            if (const usize* existing = m_index.Find(key))
            {
                // Re-adding a variant overwrites in place - a pushed duplicate would stay
                // orphaned in m_entries and be serialized (and Count()ed) twice.
                m_entries[*existing] = Move(e);
                return;
            }
            m_index.InsertOrAssign(key, m_entries.Size());
            m_entries.PushBack(Move(e));
        }

        // Record a stage's declared variant mask (from its `// draconic:variants` directive) so the
        // dist runtime can canonicalize a request (flags & mask) onto a variant that was cooked. The
        // cook calls this once per stage; absent => None (single-variant).
        void AddDeclaredMask(StringView name, ShaderStage stage, ShaderFlags mask)
        {
            m_declaredMasks.InsertOrAssign(MaskKey(ShaderNameHash(name), stage),
                                           static_cast<u32>(mask));
        }

        // The declared mask for (name, stage); None if the stage declared none / is absent.
        [[nodiscard]] ShaderFlags DeclaredMask(u64 nameHash, ShaderStage stage) const
        {
            const u32* m = m_declaredMasks.Find(MaskKey(nameHash, stage));
            return (m != nullptr) ? static_cast<ShaderFlags>(*m) : ShaderFlags::None;
        }

        // Look up a cooked blob; null when absent (a cook-coverage bug at runtime).
        [[nodiscard]] const Array<byte>* Find(u64 nameHash, ShaderStage stage, ShaderFlags flags,
                                              CookedShaderFormat format) const
        {
            const u64 key = CombineKey(nameHash, stage, flags, format);
            const usize* idx = m_index.Find(key);
            return (idx != nullptr) ? &m_entries[*idx].blob : nullptr;
        }
        [[nodiscard]] const Array<byte>* Find(StringView name, ShaderStage stage, ShaderFlags flags,
                                              CookedShaderFormat format) const
        {
            return Find(ShaderNameHash(name), stage, flags, format);
        }

        // Diagnostic: visit every (flags, format) variant the pack holds for a (name, stage). Used by
        // the runtime miss path to report what WAS cooked vs the (canonical) request that missed, so a
        // cook-coverage gap shows its shape (wrong format / flags / mask) instead of just "missing".
        template <class Fn>
        void ForEachVariant(u64 nameHash, ShaderStage stage, Fn&& fn) const
        {
            for (const Entry& e : m_entries)
            {
                if (e.nameHash == nameHash && e.stage == stage)
                {
                    fn(e.flags, e.format);
                }
            }
        }

        // Distinct shader names in the pack (for provider enumeration / tooling).
        void CollectNames(Array<String>& out) const
        {
            for (const auto& kv : m_names)
            {
                out.PushBack(kv.value);
            }
        }

        [[nodiscard]] usize Count() const noexcept { return m_entries.Size(); }
        [[nodiscard]] bool IsEmpty() const noexcept { return m_entries.IsEmpty(); }

        // --- Serialization (binary) ---------------------------------------
        [[nodiscard]] Status Write(IStream& out) const
        {
            BinaryWriter w(out);
            w.Write<u32>(kMagic);
            w.Write<u32>(kVersion);

            w.Write<u32>(static_cast<u32>(m_names.Size()));
            for (const auto& kv : m_names)
            {
                w.Write<u64>(kv.key);
                w.WriteString(kv.value.AsView());
            }

            w.Write<u32>(static_cast<u32>(m_declaredMasks.Size()));
            for (const auto& kv : m_declaredMasks)
            {
                w.Write<u64>(kv.key);
                w.Write<u32>(kv.value);
            }

            w.Write<u32>(static_cast<u32>(m_entries.Size()));
            for (const Entry& e : m_entries)
            {
                w.Write<u64>(e.nameHash);
                w.Write<u32>(static_cast<u32>(e.stage));
                w.Write<u32>(static_cast<u32>(e.flags));
                w.Write<u32>(static_cast<u32>(e.format));
                w.Write<u32>(static_cast<u32>(e.blob.Size()));
                w.WriteBytes(e.blob.Data(), e.blob.Size());
            }
            return w.IsOk() ? Status{} : Status{ErrorCode::Internal};
        }

        [[nodiscard]] Status Read(IStream& in)
        {
            m_entries.Clear();
            m_index.Clear();
            m_names.Clear();
            m_declaredMasks.Clear();

            BinaryReader r(in);
            u32 magic = 0;
            u32 version = 0;
            r.Read(magic);
            r.Read(version);
            if (!r.IsOk() || magic != kMagic || version != kVersion)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            // Count guard: every count/length read below is bounded by the bytes actually
            // remaining in the stream, so a truncated/corrupt pack fails cleanly instead of
            // attempting a multi-GB Resize on a garbage length.
            const i64 streamSize = in.Size();
            const auto remaining = [&]() -> u64
            {
                const i64 pos = in.Tell();
                return (streamSize >= 0 && pos >= 0 && streamSize > pos)
                           ? static_cast<u64>(streamSize - pos)
                           : 0ull;
            };

            u32 nameCount = 0;
            r.Read(nameCount);
            if (nameCount > remaining() / 12) // hash(8) + length(4) minimum per record
            {
                return Status{ErrorCode::InvalidArgument};
            }
            for (u32 i = 0; i < nameCount && r.IsOk(); ++i)
            {
                u64 nameHash = 0;
                String name;
                r.Read(nameHash);
                r.ReadString(name);
                m_names.InsertOrAssign(nameHash, Move(name));
            }

            u32 maskCount = 0;
            r.Read(maskCount);
            if (maskCount > remaining() / 12) // key(8) + mask(4) per record
            {
                return Status{ErrorCode::InvalidArgument};
            }
            for (u32 i = 0; i < maskCount && r.IsOk(); ++i)
            {
                u64 maskKey = 0;
                u32 mask = 0;
                r.Read(maskKey);
                r.Read(mask);
                m_declaredMasks.InsertOrAssign(maskKey, mask);
            }

            u32 entryCount = 0;
            r.Read(entryCount);
            if (entryCount > remaining() / 24) // fixed header bytes per entry
            {
                return Status{ErrorCode::InvalidArgument};
            }
            for (u32 i = 0; i < entryCount && r.IsOk(); ++i)
            {
                Entry e;
                u32 stage = 0, flags = 0, format = 0, blobLen = 0;
                r.Read(e.nameHash);
                r.Read(stage);
                r.Read(flags);
                r.Read(format);
                r.Read(blobLen);
                e.stage = static_cast<ShaderStage>(stage);
                e.flags = static_cast<ShaderFlags>(flags);
                e.format = static_cast<CookedShaderFormat>(format);
                if (!r.IsOk() || blobLen > remaining())
                {
                    return Status{ErrorCode::InvalidArgument}; // truncated/corrupt blob length
                }
                e.blob.Resize(blobLen);
                if (blobLen > 0)
                {
                    r.ReadBytes(e.blob.Data(), blobLen);
                }
                if (!r.IsOk())
                {
                    break;
                }
                const u64 key = CombineKey(e.nameHash, e.stage, e.flags, e.format);
                m_index.InsertOrAssign(key, m_entries.Size());
                m_entries.PushBack(Move(e));
            }
            return r.IsOk() ? Status{} : Status{ErrorCode::Internal};
        }

    private:
        static constexpr u32 kMagic = 0x4b505344u;   // "DSPK" little-endian
        static constexpr u32 kVersion = 2u;          // v2 added the declared-mask table

        [[nodiscard]] static u64 MaskKey(u64 nameHash, ShaderStage stage) noexcept
        {
            return (nameHash << 4) ^ static_cast<u64>(stage);
        }

        struct Entry
        {
            u64 nameHash = 0;
            ShaderStage stage = ShaderStage::Vertex;
            ShaderFlags flags = ShaderFlags::None;
            CookedShaderFormat format = CookedShaderFormat::SpirV;
            Array<byte> blob;
        };

        [[nodiscard]] static u64 CombineKey(u64 nameHash, ShaderStage stage, ShaderFlags flags,
                                            CookedShaderFormat format) noexcept
        {
            u64 k = nameHash * 1099511628211ull;
            k ^= (static_cast<u64>(stage) << 16) | (static_cast<u64>(flags) << 3) |
                 static_cast<u64>(format);
            return k;
        }

        Array<Entry> m_entries;
        HashMap<u64, usize> m_index;         // combined key -> index into m_entries
        HashMap<u64, String> m_names;        // nameHash -> name (enumeration)
        HashMap<u64, u32> m_declaredMasks;   // MaskKey(nameHash, stage) -> ShaderFlags bits
    };
}
