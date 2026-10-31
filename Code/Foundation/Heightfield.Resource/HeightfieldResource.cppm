// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Heightfield.Resource - the `foundation.heightfield.resource` module.
///
/// The heightfield as a referenceable, cooked resource. HeightfieldSource is the small serialized
/// metadata (size, world footprint, Y range); the u16 sample bulk rides a separate "heights" data
/// stream rather than inline in the serialized object, as ImageResource's "pixels" does.
/// HeightfieldFactory reads the metadata and the stream and builds the runtime Heightfield
/// (foundation.heightfield). Terrain, the physics collider and the nav bake all resolve
/// Ref<Heightfield> to this product.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.heightfield.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.heightfield;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::heightfield
{
    /// The name of the sidecar stream carrying the raw u16 samples.
    inline constexpr StringView kHeightStream = u8"heights";
    /// The per-sample hole plane (one byte per sample, 0 solid, 255 cut), always written beside
    /// the heights (an all-zero plane is the common file; one layout, never optional).
    inline constexpr StringView kHoleStream = u8"holes";

    /// Cooked heightfield metadata: the grid parameters. The u16 sample bulk is not here; it rides
    /// the `kHeightStream` data stream (see the module doc).
    class HeightfieldSource final : public ISerializable
    {
        RTTI_OBJECT(HeightfieldSource, ISerializable)
    public:
        i32 size = 0;
        Float2 worldSize{0.0f, 0.0f};
        f32 minY = 0.0f;
        f32 maxY = 0.0f;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "size", size);
            foundation::core::Serialize(ar, "worldSize", worldSize);
            foundation::core::Serialize(ar, "minY", minY);
            foundation::core::Serialize(ar, "maxY", maxY);
        }

        /// Capture a runtime Heightfield's metadata into this source (for cooking). The samples are
        /// written separately via HeightBlob and WriteData(kHeightStream, ...).
        static void FromHeightfield(const Heightfield& hf, HeightfieldSource& out)
        {
            out.size = hf.Size();
            out.worldSize = hf.WorldSize();
            out.minY = hf.MinY();
            out.maxY = hf.MaxY();
        }

        /// The raw sample bytes of a heightfield, to feed WriteData(kHeightStream, ...).
        [[nodiscard]] static Span<const byte> HeightBlob(const Heightfield& hf) noexcept
        {
            const Span<const Height> samples = hf.Samples();
            return Span<const byte>(reinterpret_cast<const byte*>(samples.Data()),
                                    samples.Size() * sizeof(Height));
        }

        /// The hole plane bytes of a heightfield, to feed WriteData(kHoleStream, ...).
        [[nodiscard]] static Span<const byte> HoleBlob(const Heightfield& hf) noexcept
        {
            const Span<const u8> holes = hf.Holes();
            return Span<const byte>(reinterpret_cast<const byte*>(holes.Data()), holes.Size());
        }

        /// Build the runtime product from this metadata and the two sidecar streams. Returns an
        /// empty grid if the cooked data is inconsistent (invalid size, a height blob that does
        /// not match size*size*2, a hole blob that does not match size*size) rather than a
        /// malformed grid. The runtime product is allocated from `allocator` (caller-owned).
        [[nodiscard]] RefPtr<Heightfield> Build(Span<const byte> heightBlob, Span<const byte> holeBlob,
                                                IAllocator& allocator) const
        {
            if (!IsValidSize(size))
            {
                return MakeRef<Heightfield>(allocator);
            }
            const usize samples = static_cast<usize>(size) * static_cast<usize>(size);
            if (heightBlob.Size() != samples * sizeof(Height) || holeBlob.Size() != samples)
            {
                return MakeRef<Heightfield>(allocator);
            }
            RefPtr<Heightfield> hf =
                MakeRef<Heightfield>(allocator, size, worldSize, minY, maxY);
            MemCopy(hf->Samples().Data(), heightBlob.Data(), samples * sizeof(Height));
            (void)hf->SetHoles(Span<const u8>(reinterpret_cast<const u8*>(holeBlob.Data()), samples));
            return hf;
        }
    };

    /// Builds a cooked heightfield (metadata object and "heights" stream) into a runtime Heightfield.
    /// Pure CPU (no GPU state: the terrain renderer owns the height texture, cached per resource), so
    /// the whole build runs on a worker.
    class HeightfieldFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required:
        // the application that registers the factory decides).
        explicit HeightfieldFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Heightfield::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &HeightfieldSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            return BuildFrom(instance);
        }
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(foundation::content::Instance& instance) override
        {
            return BuildFrom(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override
        {
            return decoded;
        }

    private:
        [[nodiscard]] RefPtr<Object> BuildFrom(foundation::content::Instance& instance) const
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            HeightfieldSource* src = Cast<HeightfieldSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            const auto readStream = [&](StringView name, Array<u8>& blob)
            {
                if (UniquePtr<IStream> stream = instance.ReadData(name))
                {
                    const i64 size = stream->Size();
                    if (size > 0)
                    {
                        blob.Resize(static_cast<usize>(size));
                        if (stream->Read(blob.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
                        {
                            blob.Clear();
                        }
                    }
                }
            };
            Array<u8> heights;
            Array<u8> holes;
            readStream(kHeightStream, heights);
            readStream(kHoleStream, holes);
            return src->Build(
                Span<const byte>(reinterpret_cast<const byte*>(heights.Data()), heights.Size()),
                Span<const byte>(reinterpret_cast<const byte*>(holes.Data()), holes.Size()),
                (*m_allocator));
        }
    
    private:
        IAllocator* m_allocator;
    };

    /// Register the heightfield resource types (product and cooked source) so the content DB can
    /// deserialize a cooked heightfield and the factory can produce it.
    inline void RegisterHeightfieldResourceTypes()
    {
        GlobalTypeRegistry().Register(Heightfield::StaticType());
        GlobalTypeRegistry().Register(HeightfieldSource::StaticType());
        RegisterSerializable<HeightfieldSource>();
    }

    // v2: the "holes" stream beside "heights". Every cooked heightfield carries both; a v1
    // product is refused and re-cooked, never migrated.
    RTTI_DEFINE_OBJECT_VERSIONED(HeightfieldSource, "rtti::heightfield", 2)
}

export namespace foundation::heightfield
{
    /// The heightfield resource module: the module the engine composition composes this
    /// library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kHeightfieldResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<Heightfield, HeightfieldSource, HeightfieldFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kHeightfieldResourceModule{
        u8"heightfield", &RegisterHeightfieldResourceTypes, kHeightfieldResourceFactories,
        sizeof(kHeightfieldResourceFactories) / sizeof(kHeightfieldResourceFactories[0])};
}
