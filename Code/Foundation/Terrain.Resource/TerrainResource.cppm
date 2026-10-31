// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Terrain.Resource - the `foundation.terrain.resource` module.
///
/// The cooked terrain resource: a bundle of references (a heightfield, the top-K splat weights,
/// an explicit base layer and an unbounded paint palette of albedo layers) and a cast-shadows
/// flag, not embedded bulk. TerrainSource is the serialized form (guids and params, as in
/// Material.Resource); TerrainFactory resolves each guid through the manager into the runtime
/// TerrainResource. TerrainComponent (engine.terrain) references Ref<TerrainResource>. The
/// heightfield is the shared source of truth: physics and nav resolve the same Ref<Heightfield>.
/// The base layer is what shows wherever painted weights do not sum to 1;
/// it is not part of the palette and is never painted directly.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.terrain.resource;

import foundation.core;
import foundation.resource;
import foundation.content;
import foundation.heightfield;
import foundation.texture.resource;

export import :splatmap; // SplatWeights (top-K rasters), brush cores, cooked source and factory

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::terrain
{
    /// The cooked terrain (serialized): the referenced resources by guid, per-layer tiling and
    /// cast-shadows. The factory resolves the guids; nothing here is bulk data.
    /// The top-K model: base, palette and weightsId, per-layer normal/ORM/height/coverage maps
    /// (nil turns that feature off) and heightBlendContrast. One layout: the current
    /// data version (a cooked terrain stamped otherwise is refused and re-cooks).
    class TerrainSource final : public ISerializable
    {
        RTTI_OBJECT(TerrainSource, ISerializable)
    public:
        Guid heightfieldId;           // the shared Ref<Heightfield> (also used by physics/nav)
        Guid weightsId;               // the SplatWeights product (nil: none, pure base)
        Guid baseAlbedoId;            // the base layer albedo (nil = white dummy)
        Guid baseNormalId;            // the base normal map (nil = flat)
        Guid baseOrmId;               // the base ORM (nil = 1,1,0 default)
        Guid baseHeightId;            // the base height/displacement map (nil = dummy)
        f32 baseTileScale = 1.0f;
        Array<Guid> paletteAlbedoIds; // paint layers, unbounded (parallel to paletteTileScales)
        Array<Guid> paletteNormalIds; // per-palette-layer normal map (nil allowed)
        Array<Guid> paletteOrmIds;    // per-palette-layer ORM (nil allowed)
        Array<Guid> paletteHeightIds; // per-palette-layer height map (nil allowed)
        Array<Guid> paletteMaskIds;   // per-palette-layer coverage/opacity map (nil allowed)
        Array<f32> paletteTileScales;
        // Height-blend soft-skirt width (consulted only when height maps are
        // present). 0..1; small gives a sharp interlock, large washes toward an equal mix.
        f32 heightBlendContrast = 0.25f;
        bool castShadows = true;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "heightfieldId", heightfieldId);
            foundation::core::Serialize(ar, "weightsId", weightsId);
            foundation::core::Serialize(ar, "baseAlbedoId", baseAlbedoId);
            foundation::core::Serialize(ar, "baseTileScale", baseTileScale);
            foundation::core::Serialize(ar, "paletteAlbedoIds", paletteAlbedoIds);
            foundation::core::Serialize(ar, "paletteTileScales", paletteTileScales);
            foundation::core::Serialize(ar, "baseNormalId", baseNormalId);
            foundation::core::Serialize(ar, "baseOrmId", baseOrmId);
            foundation::core::Serialize(ar, "paletteNormalIds", paletteNormalIds);
            foundation::core::Serialize(ar, "paletteOrmIds", paletteOrmIds);
            foundation::core::Serialize(ar, "baseHeightId", baseHeightId);
            foundation::core::Serialize(ar, "paletteHeightIds", paletteHeightIds);
            foundation::core::Serialize(ar, "heightBlendContrast", heightBlendContrast);
            foundation::core::Serialize(ar, "paletteMaskIds", paletteMaskIds);
            foundation::core::Serialize(ar, "castShadows", castShadows);
        }
    };

    /// The cook-built paint-palette texel data: every palette albedo resized to one common slice
    /// size and mip-chained, concatenated slice-major (slice 0 mips 0..M-1, slice 1 mips ...).
    /// CPU-side, like the heightfield and weights: engine.terrain derives and caches the GPU
    /// Texture2DArray keyed by `uid`. It travels in the terrain's own cooked instance as the
    /// `kPaletteStream` sidecar, with no extra product guid.
    class TerrainPaletteData final : public Object
    {
        RTTI_OBJECT(TerrainPaletteData, Object)
    public:
        const u64 uid = SplatWeights::NextUid(); // shared uid domain; cache key (never a pointer)

        u32 sliceSize = 0;  // square slice side (power of two)
        u32 mipCount = 0;   // mips per slice (down to 1x1)
        u32 sliceCount = 0; // == the palette layer count at cook time
        Array<u8> texels;   // albedo array: RGBA8, slice-major, each slice its full mip chain
        // The normal and ORM arrays, built on demand: empty means absent (no palette
        // layer used that map, so the renderer binds a 1x1 dummy). Present arrays share the albedo's
        // sliceSize/mipCount/sliceCount geometry.
        Array<u8> normalTexels;
        Array<u8> ormTexels;
        // The height (displacement) array, built on demand: empty means absent (no
        // palette layer used a height map, so the renderer binds a 1x1 mid-height dummy). Same geometry
        // as albedo; read through the red channel.
        Array<u8> heightTexels;
        // The coverage (opacity) array, built on demand: empty means absent (no
        // palette layer used a mask, so a 1x1 opaque dummy binds). Same geometry; read through red.
        Array<u8> maskTexels;

        /// Total bytes of one slice's full mip chain for `sliceSize`/`mipCount`.
        [[nodiscard]] static usize SliceBytes(u32 sliceSize, u32 mipCount) noexcept
        {
            usize total = 0;
            u32 dim = sliceSize;
            for (u32 m = 0; m < mipCount; ++m)
            {
                total += static_cast<usize>(dim) * dim * 4u;
                dim = dim > 1 ? dim / 2 : 1;
            }
            return total;
        }

        /// One array's expected byte size at this geometry.
        [[nodiscard]] usize ArrayBytes() const noexcept
        {
            return SliceBytes(sliceSize, mipCount) * sliceCount;
        }
        [[nodiscard]] bool IsValid() const noexcept
        {
            return sliceSize > 0 && mipCount > 0 && sliceCount > 0 && texels.Size() == ArrayBytes();
        }
        [[nodiscard]] bool HasNormal() const noexcept
        {
            return IsValid() && normalTexels.Size() == ArrayBytes();
        }
        [[nodiscard]] bool HasOrm() const noexcept
        {
            return IsValid() && ormTexels.Size() == ArrayBytes();
        }
        [[nodiscard]] bool HasHeight() const noexcept
        {
            return IsValid() && heightTexels.Size() == ArrayBytes();
        }
        [[nodiscard]] bool HasMask() const noexcept
        {
            return IsValid() && maskTexels.Size() == ArrayBytes();
        }
    };

    /// The terrain cooked instance's palette sidecar streams (see TerrainPaletteData). The normal,
    /// ORM, height and mask streams are absent when no palette layer uses that map.
    inline constexpr StringView kPaletteStream = u8"palette";
    inline constexpr StringView kPaletteNormalStream = u8"palette.normal";
    inline constexpr StringView kPaletteOrmStream = u8"palette.orm";
    inline constexpr StringView kPaletteHeightStream = u8"palette.height";
    inline constexpr StringView kPaletteMaskStream = u8"palette.mask";

    /// The runtime terrain product: the resolved resource handles the renderer draws with. The
    /// heightfield drives geometry (through foundation.terrain's chunk model) and the shared collision.
    class TerrainResource final : public Object
    {
        RTTI_OBJECT(TerrainResource, Object)
    public:
        struct Layer
        {
            // Ref (not Proxy): the cooked factory binds it as a proxy, but in-memory or procedural
            // terrain (playgrounds, tests, runtime generation) can assign a product directly.
            Ref<texture::Texture> albedo;
            Ref<texture::Texture> normal; // tangent-space normal map (nil = flat)
            Ref<texture::Texture> orm;    // R=AO G=roughness B=metallic (nil = 1,1,0 default)
            Ref<texture::Texture> height; // displacement map for height-blend (nil = dummy); .r used
            Ref<texture::Texture> mask;   // coverage/opacity (palette only; nil = opaque); .r used
            f32 tileScale = 1.0f;         // shared by all maps of this layer
        };

        Ref<heightfield::Heightfield> heightfield;
        Ref<SplatWeights> weights; // the top-K paint rasters (nil = pure base)
        Layer base;                // shows wherever paint doesn't sum to 1; never painted
        Array<Layer> palette;      // the paint layers, unbounded (an 8-bit index, so <= 256 used)
        // The cook-built palette texel array (null = no palette or an in-memory terrain that
        // assigns it directly); engine.terrain uploads it as the Texture2DArray.
        RefPtr<TerrainPaletteData> paletteData;
        // Height-blend soft-skirt width, copied from the source; the renderer
        // feeds it to the shader only when height maps are present.
        f32 heightBlendContrast = 0.25f;
        bool castShadows = true;

        [[nodiscard]] u32 PaletteCount() const noexcept
        {
            return static_cast<u32>(palette.Size());
        }
    };

    /// Resolves a TerrainSource into a runtime TerrainResource, binding each referenced resource through the
    /// manager (recording the dependency edges, so a heightfield or texture reload cascades). A missing
    /// sub-resource (not cooked, or no GPU factory in headless tools) leaves that handle unbound.
    class TerrainFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required;
        // the application that registers the factory decides).
        explicit TerrainFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override { return &TerrainResource::StaticType(); }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &TerrainSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            TerrainSource* src = Cast<TerrainSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            RefPtr<TerrainResource> terrain = MakeRef<TerrainResource>((*m_allocator));
            terrain->castShadows = src->castShadows;
            terrain->heightBlendContrast = src->heightBlendContrast;
            // Stamp each ref's serialized identity (its source guid) as well as binding the proxy:
            // Ref<T>::id exists to carry this, and factory-built products must round-trip their
            // sub-ref ids (serializing one otherwise writes nil); the editor also resolves the
            // heightfield asset from TerrainResource.heightfield.id (product guid == source guid).
            if (!src->heightfieldId.IsNil())
            {
                terrain->heightfield.SetProxy(
                    manager.Bind<heightfield::Heightfield>(src->heightfieldId));
                terrain->heightfield.SetId(src->heightfieldId);
            }
            if (!src->weightsId.IsNil())
            {
                terrain->weights.SetProxy(manager.Bind<SplatWeights>(src->weightsId));
                terrain->weights.SetId(src->weightsId);
            }
            // Bind and stamp a texture ref from its source guid (nil leaves it unbound: a dummy).
            auto bind = [&manager](Ref<texture::Texture>& ref, const Guid& id)
            {
                if (!id.IsNil())
                {
                    ref.SetProxy(manager.Bind<texture::Texture>(id));
                    ref.SetId(id);
                }
            };
            terrain->base.tileScale = src->baseTileScale;
            bind(terrain->base.albedo, src->baseAlbedoId);
            bind(terrain->base.normal, src->baseNormalId);
            bind(terrain->base.orm, src->baseOrmId);
            bind(terrain->base.height, src->baseHeightId);
            for (usize i = 0; i < src->paletteAlbedoIds.Size(); ++i)
            {
                TerrainResource::Layer layer;
                layer.tileScale =
                    (i < src->paletteTileScales.Size()) ? src->paletteTileScales[i] : 1.0f;
                bind(layer.albedo, src->paletteAlbedoIds[i]);
                if (i < src->paletteNormalIds.Size())
                {
                    bind(layer.normal, src->paletteNormalIds[i]);
                }
                if (i < src->paletteOrmIds.Size())
                {
                    bind(layer.orm, src->paletteOrmIds[i]);
                }
                if (i < src->paletteHeightIds.Size())
                {
                    bind(layer.height, src->paletteHeightIds[i]);
                }
                if (i < src->paletteMaskIds.Size())
                {
                    bind(layer.mask, src->paletteMaskIds[i]);
                }
                terrain->palette.PushBack(Move(layer));
            }
            // The cook-built palette texel arrays: albedo (kPaletteStream) and the optional
            // sidecars on this same cooked instance (each a {sliceSize, mipCount, sliceCount} header
            // and texels; absent means no array).
            u32 hdr[3] = {0, 0, 0};
            Array<u8> texels;
            if (ReadArrayStream(instance, kPaletteStream, hdr, texels))
            {
                RefPtr<TerrainPaletteData> palette = MakeRef<TerrainPaletteData>((*m_allocator));
                palette->sliceSize = hdr[0];
                palette->mipCount = hdr[1];
                palette->sliceCount = hdr[2];
                palette->texels = Move(texels);
                if (palette->IsValid())
                {
                    const usize expect = palette->ArrayBytes();
                    u32 nhdr[3] = {0, 0, 0};
                    Array<u8> ntex;
                    if (ReadArrayStream(instance, kPaletteNormalStream, nhdr, ntex) &&
                        nhdr[0] == hdr[0] && nhdr[1] == hdr[1] && nhdr[2] == hdr[2] &&
                        ntex.Size() == expect)
                    {
                        palette->normalTexels = Move(ntex);
                    }
                    u32 ohdr[3] = {0, 0, 0};
                    Array<u8> otex;
                    if (ReadArrayStream(instance, kPaletteOrmStream, ohdr, otex) &&
                        ohdr[0] == hdr[0] && ohdr[1] == hdr[1] && ohdr[2] == hdr[2] &&
                        otex.Size() == expect)
                    {
                        palette->ormTexels = Move(otex);
                    }
                    u32 hhdr[3] = {0, 0, 0};
                    Array<u8> htex;
                    if (ReadArrayStream(instance, kPaletteHeightStream, hhdr, htex) &&
                        hhdr[0] == hdr[0] && hhdr[1] == hdr[1] && hhdr[2] == hdr[2] &&
                        htex.Size() == expect)
                    {
                        palette->heightTexels = Move(htex);
                    }
                    u32 mhdr[3] = {0, 0, 0};
                    Array<u8> mtex;
                    if (ReadArrayStream(instance, kPaletteMaskStream, mhdr, mtex) &&
                        mhdr[0] == hdr[0] && mhdr[1] == hdr[1] && mhdr[2] == hdr[2] &&
                        mtex.Size() == expect)
                    {
                        palette->maskTexels = Move(mtex);
                    }
                    terrain->paletteData = Move(palette);
                }
            }
            return terrain;
        }

    private:
        // Read a "{sliceSize, mipCount, sliceCount} u32 header + texels" palette stream. Returns false
        // if the stream is absent or truncated; on success fills `outHeader` and `outTexels`.
        [[nodiscard]] static bool ReadArrayStream(foundation::content::Instance& instance,
                                                  StringView name, u32 outHeader[3],
                                                  Array<u8>& outTexels)
        {
            UniquePtr<IStream> stream = instance.ReadData(name);
            if (!stream)
            {
                return false;
            }
            const i64 size = stream->Size();
            if (size <= static_cast<i64>(sizeof(u32) * 3))
            {
                return false;
            }
            if (stream->Read(outHeader, sizeof(u32) * 3) != sizeof(u32) * 3)
            {
                return false;
            }
            const usize texelBytes = static_cast<usize>(size) - sizeof(u32) * 3;
            outTexels.Resize(texelBytes);
            return stream->Read(outTexels.Data(), static_cast<u64>(texelBytes)) == texelBytes;
        }
    
    private:
        IAllocator* m_allocator;
    };

    /// Register the terrain resource types (product and cooked source) for load.
    inline void RegisterTerrainResourceTypes()
    {
        GlobalTypeRegistry().Register(TerrainResource::StaticType());
        GlobalTypeRegistry().Register(TerrainPaletteData::StaticType());
        GlobalTypeRegistry().Register(TerrainSource::StaticType());
        RegisterSerializable<TerrainSource>();
    }

    RTTI_DEFINE_OBJECT(TerrainResource, "rtti::terrain")
    RTTI_DEFINE_OBJECT(TerrainPaletteData, "rtti::terrain")
    RTTI_DEFINE_OBJECT_VERSIONED(TerrainSource, "rtti::terrain", 5)
}

export namespace foundation::terrain
{
    /// Both halves of this library's type registration (the terrain bundle and the splatmap).
    inline void RegisterTerrainResourceModuleTypes()
    {
        RegisterTerrainResourceTypes();
        RegisterSplatmapResourceTypes();
    }

    /// The terrain resource module: what the engine composition composes this library's
    /// factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kTerrainResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<TerrainResource, TerrainSource, TerrainFactory>(),
        foundation::resource::FactoryWithAllocator<SplatWeights, SplatWeightsSource, SplatWeightsFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kTerrainResourceModule{
        u8"terrain", &RegisterTerrainResourceModuleTypes, kTerrainResourceFactories,
        sizeof(kTerrainResourceFactories) / sizeof(kTerrainResourceFactories[0])};
}
