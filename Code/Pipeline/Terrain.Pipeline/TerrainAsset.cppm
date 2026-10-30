// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Terrain - the `terrain.pipeline` module.
//
// Tooling: the source TerrainAsset (references a heightfield asset + the splat weights + an
// explicit BASE layer + an unbounded paint PALETTE of albedo textures + tiling + cast-shadows)
// and the builder that cooks it into the Terrain resource. It does NOT import a heightmap - that
// is Heightfield.Pipeline; a terrain REFERENCES an existing heightfield asset. The cook is a
// reference pass-through (asset ids == cooked product ids). Never linked by the runtime.
//
// Splat model = top-K: SplatmapAsset carries TWO sidecars - "pixels"
// (the 4 x u8 slot weights) + "indices" (the 4 x u8 palette indices); both or neither.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module terrain.pipeline;

import foundation.core;
import foundation.vfs;
import pipeline.core;
import pipeline.importer; // IFileImporter + ImportContext + CopyIntoSources (splatmap PNG import)
import foundation.image;
import foundation.image.io; // LoadImageFromMemory (RGBA8 decode - reuses the image decoder)
import foundation.terrain.resource;
import foundation.content;
import texture.pipeline; // TextureAsset (decoding palette albedos for the array cook)

using namespace foundation::core;
namespace content = foundation::content;

export namespace pipeline
{
    using foundation::terrain::TerrainResource;
    using foundation::terrain::TerrainSource;

    // Source asset: references a heightfield + the splat weights + the BASE layer + the paint
    // palette (by asset guid, which equal their cooked product guids) + tiling + cast-shadows.
    // The top-K model (base + palette + weightsId), one layout per data version.
    class TerrainAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(TerrainAsset, pipeline::Asset)
    public:
        Guid heightfieldId;           // the referenced heightfield asset (shared with physics/nav)
        Guid weightsId;               // the SplatmapAsset (top-K weights); nil = none (pure base)
        Guid baseAlbedoId;            // the BASE layer albedo (nil = white dummy)
        Guid baseNormalId;            // the BASE normal map (nil = flat); DataVersion 3
        Guid baseOrmId;               // the BASE ORM (nil = 1,1,0 default); DataVersion 3
        Guid baseHeightId;            // the BASE height/displacement map (nil = dummy); DataVersion 4
        f32 baseTileScale = 1.0f;
        Array<Guid> paletteAlbedoIds; // paint layers, unbounded (parallel to paletteTileScales)
        Array<Guid> paletteNormalIds; // per-palette-layer normal map (nil allowed); DataVersion 3
        Array<Guid> paletteOrmIds;    // per-palette-layer ORM (nil allowed); DataVersion 3
        Array<Guid> paletteHeightIds; // per-palette-layer height map (nil allowed); DataVersion 4
        Array<Guid> paletteMaskIds;   // per-palette-layer coverage/opacity map (nil allowed); DV 5
        Array<f32> paletteTileScales;
        i32 paletteTextureSize = 1024; // common Texture2DArray slice size (authoring setting)
        f32 heightBlendContrast = 0.25f; // height-blend soft-skirt width; DataVersion 4
        bool castShadows = true;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (unused; terrain references sub-assets)
            foundation::core::Serialize(ar, "heightfieldId", heightfieldId);
            foundation::core::Serialize(ar, "weightsId", weightsId);
            foundation::core::Serialize(ar, "baseAlbedoId", baseAlbedoId);
            foundation::core::Serialize(ar, "baseTileScale", baseTileScale);
            foundation::core::Serialize(ar, "paletteAlbedoIds", paletteAlbedoIds);
            foundation::core::Serialize(ar, "paletteTileScales", paletteTileScales);
            foundation::core::Serialize(ar, "paletteTextureSize", paletteTextureSize);
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

    // --- palette-array cook helpers (RGBA8, CPU; exported for tests) ---------------------------

    /// Bilinear-resize an RGBA8 image onto dstW x dstH (the heightfield-resample precedent).
    inline void ResizeRgba8Bilinear(Span<const u8> src, u32 srcW, u32 srcH, Span<u8> dst, u32 dstW,
                                    u32 dstH)
    {
        if (src.IsEmpty() || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0)
        {
            return;
        }
        for (u32 y = 0; y < dstH; ++y)
        {
            const f32 v = (dstH > 1) ? static_cast<f32>(y) / static_cast<f32>(dstH - 1) *
                                           static_cast<f32>(srcH - 1)
                                     : 0.0f;
            const u32 y0 = static_cast<u32>(v);
            const u32 y1 = Min(y0 + 1, srcH - 1);
            const f32 fy = v - static_cast<f32>(y0);
            for (u32 x = 0; x < dstW; ++x)
            {
                const f32 u = (dstW > 1) ? static_cast<f32>(x) / static_cast<f32>(dstW - 1) *
                                               static_cast<f32>(srcW - 1)
                                         : 0.0f;
                const u32 x0 = static_cast<u32>(u);
                const u32 x1 = Min(x0 + 1, srcW - 1);
                const f32 fx = u - static_cast<f32>(x0);
                for (u32 c = 0; c < 4; ++c)
                {
                    const f32 p00 = src[(static_cast<usize>(y0) * srcW + x0) * 4 + c];
                    const f32 p10 = src[(static_cast<usize>(y0) * srcW + x1) * 4 + c];
                    const f32 p01 = src[(static_cast<usize>(y1) * srcW + x0) * 4 + c];
                    const f32 p11 = src[(static_cast<usize>(y1) * srcW + x1) * 4 + c];
                    const f32 top = p00 + (p10 - p00) * fx;
                    const f32 bottom = p01 + (p11 - p01) * fx;
                    dst[(static_cast<usize>(y) * dstW + x) * 4 + c] =
                        static_cast<u8>(Clamp(top + (bottom - top) * fy + 0.5f, 0.0f, 255.0f));
                }
            }
        }
    }

    /// Append `level`'s 2x2 box-filtered half-size mip after it; returns the new dimension.
    inline u32 BoxHalveRgba8(Span<const u8> src, u32 dim, Span<u8> dst)
    {
        const u32 half = dim > 1 ? dim / 2 : 1;
        for (u32 y = 0; y < half; ++y)
        {
            const u32 sy0 = Min(y * 2, dim - 1);
            const u32 sy1 = Min(y * 2 + 1, dim - 1);
            for (u32 x = 0; x < half; ++x)
            {
                const u32 sx0 = Min(x * 2, dim - 1);
                const u32 sx1 = Min(x * 2 + 1, dim - 1);
                for (u32 c = 0; c < 4; ++c)
                {
                    const u32 sum = src[(static_cast<usize>(sy0) * dim + sx0) * 4 + c] +
                                    src[(static_cast<usize>(sy0) * dim + sx1) * 4 + c] +
                                    src[(static_cast<usize>(sy1) * dim + sx0) * 4 + c] +
                                    src[(static_cast<usize>(sy1) * dim + sx1) * 4 + c];
                    dst[(static_cast<usize>(y) * half + x) * 4 + c] =
                        static_cast<u8>((sum + 2) / 4);
                }
            }
        }
        return half;
    }

    /// Box-halve an sRGB-ENCODED RGBA8 level averaging the colour channels in LINEAR space
    /// (decode -> average -> re-encode; alpha is linear and averages as-is). Averaging sRGB bytes
    /// directly darkens mips - a 50% black/white checker must average to sRGB ~188, not 128 (the
    /// texture cook's rule). Used for the ALBEDO palette array, which
    /// uploads as RGBA8UnormSrgb; the linear normal/ORM arrays keep the plain box filter.
    inline u32 BoxHalveRgba8SrgbAware(Span<const u8> src, u32 dim, Span<u8> dst)
    {
        // 256-entry sRGB -> linear decode table (built once).
        static const auto kToLinear = []
        {
            struct Table { f32 v[256]; };
            Table t{};
            for (u32 i = 0; i < 256; ++i)
            {
                const f32 c = static_cast<f32>(i) / 255.0f;
                t.v[i] = (c <= 0.04045f) ? (c / 12.92f) : Pow((c + 0.055f) / 1.055f, 2.4f);
            }
            return t;
        }();
        const auto encode = [](f32 linear) -> u8
        {
            const f32 c = (linear <= 0.0031308f) ? (linear * 12.92f)
                                                 : (1.055f * Pow(linear, 1.0f / 2.4f) - 0.055f);
            return static_cast<u8>(Clamp(c * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        const u32 half = dim > 1 ? dim / 2 : 1;
        for (u32 y = 0; y < half; ++y)
        {
            const u32 sy0 = Min(y * 2, dim - 1);
            const u32 sy1 = Min(y * 2 + 1, dim - 1);
            for (u32 x = 0; x < half; ++x)
            {
                const u32 sx0 = Min(x * 2, dim - 1);
                const u32 sx1 = Min(x * 2 + 1, dim - 1);
                const usize at[4] = {(static_cast<usize>(sy0) * dim + sx0) * 4,
                                     (static_cast<usize>(sy0) * dim + sx1) * 4,
                                     (static_cast<usize>(sy1) * dim + sx0) * 4,
                                     (static_cast<usize>(sy1) * dim + sx1) * 4};
                for (u32 c = 0; c < 3; ++c)
                {
                    const f32 avg = (kToLinear.v[src[at[0] + c]] + kToLinear.v[src[at[1] + c]] +
                                     kToLinear.v[src[at[2] + c]] + kToLinear.v[src[at[3] + c]]) *
                                    0.25f;
                    dst[(static_cast<usize>(y) * half + x) * 4 + c] = encode(avg);
                }
                const u32 alphaSum = src[at[0] + 3] + src[at[1] + 3] + src[at[2] + 3] +
                                     src[at[3] + 3];
                dst[(static_cast<usize>(y) * half + x) * 4 + 3] =
                    static_cast<u8>((alphaSum + 2) / 4);
            }
        }
        return half;
    }

    // Cooks a TerrainAsset -> Terrain resource (a reference pass-through for the guids, PLUS the
    // paint-palette Texture2DArray texels: every palette albedo's source pixels decoded, resized
    // to the common slice size, mip-chained, and packed into the terrain's own cooked instance as
    // the `kPaletteStream` sidecar - hash-chained via `reads`, so editing an albedo re-cooks).
    class TerrainAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &TerrainAsset::StaticType();
        }
        // Cooked instance type = the SERIALIZED form the cook stamps + ReadObject reconstructs
        // (TerrainSource), NOT the runtime TerrainResource. TerrainFactory.ProductType() is the
        // runtime TerrainResource that Bind matches (the two differ - Texture convention).
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &TerrainSource::StaticType();
        }
        // 3: the cooked payload moved to the top-K model (TerrainSource v2) - force a re-cook.
        // 4: palette albedos decode through ctx.sourceDb (v3 cooked every slice WHITE) - re-cook.
        // 5: per-layer normal + ORM arrays - re-cook.
        // 6: albedo-array mips average in linear space (v5 averaged sRGB bytes) - re-cook.
        // 7: per-layer height array + heightBlendContrast - re-cook.
        // 8: per-layer coverage/opacity mask array - re-cook.
        // 9: on-demand sidecars are DELETED when their map is removed (was: stale sidecar persisted
        //    and kept loading) - force a re-cook so existing terrains shed any stale arrays.
        [[nodiscard]] u32 Version() const override { return 9; }

        // The palette pack READS every palette albedo/normal/ORM/height/mask content (hash-chained:
        // editing any re-cooks the terrain's arrays); base/heightfield/weights are runtime refs only.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const TerrainAsset& ta = static_cast<const TerrainAsset&>(asset);
            auto chain = [&out](const Array<Guid>& ids)
            {
                for (usize i = 0; i < ids.Size(); ++i)
                {
                    if (!ids[i].IsNil())
                    {
                        out.reads.PushBack(ids[i]);
                    }
                }
            };
            chain(ta.paletteAlbedoIds);
            chain(ta.paletteNormalIds);
            chain(ta.paletteOrmIds);
            chain(ta.paletteHeightIds);
            chain(ta.paletteMaskIds);
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const TerrainAsset& ta = static_cast<const TerrainAsset&>(asset); // AssetType()-guarded
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            TerrainSource src;
            src.heightfieldId = ta.heightfieldId;
            src.weightsId = ta.weightsId;
            src.baseAlbedoId = ta.baseAlbedoId;
            src.baseNormalId = ta.baseNormalId;
            src.baseOrmId = ta.baseOrmId;
            src.baseHeightId = ta.baseHeightId;
            src.baseTileScale = ta.baseTileScale;
            src.paletteAlbedoIds = ta.paletteAlbedoIds;
            src.paletteNormalIds = ta.paletteNormalIds;
            src.paletteOrmIds = ta.paletteOrmIds;
            src.paletteHeightIds = ta.paletteHeightIds;
            src.paletteMaskIds = ta.paletteMaskIds;
            src.paletteTileScales = ta.paletteTileScales;
            src.heightBlendContrast = ta.heightBlendContrast;
            src.castShadows = ta.castShadows;
            const Status wrote = ctx.output->WriteObject(src);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            return CookPaletteArray(ta, ctx);
        }

    private:
        // Decode one layer texture's SOURCE pixels as RGBA8: a TextureAsset's file (image decoder)
        // or its embedded "pixels" sidecar. Missing/undecodable -> leaves outPixels EMPTY (the caller
        // fills a per-map default 1x1 slice: white albedo / flat normal / default ORM). Reads the
        // SOURCE db: at cook time ctx.db holds cooked texture PRODUCTS (possibly compressed), whose
        // ReadObject is not a TextureAsset - resolving there fails to decode.
        static void DecodeTextureRgba8(pipeline::AssetBuildContext& ctx, const Guid& id,
                                       Array<u8>& outPixels, u32& outW, u32& outH)
        {
            outPixels.Clear();
            outW = 1;
            outH = 1;
            foundation::content::IContentDatabase* db =
                ctx.sourceDb != nullptr ? ctx.sourceDb : ctx.db; // single-DB tools set only db
            content::Instance* inst =
                (db != nullptr && !id.IsNil()) ? db->GetInstance(id) : nullptr;
            if (inst != nullptr)
            {
                RefPtr<ISerializable> object = inst->ReadObject();
                if (auto* tex = Cast<TextureAsset>(object.Get()))
                {
                    if (!tex->fileName.View().IsEmpty())
                    {
                        if (Result<Array<byte>> bytes = ReadSourceBytes(ctx, tex->fileName.View());
                            bytes.HasValue())
                        {
                            foundation::image::Image img;
                            if (foundation::image::io::LoadImageFromMemory(
                                    Span<const u8>(
                                        reinterpret_cast<const u8*>(bytes.Value().Data()),
                                        bytes.Value().Size()),
                                    img)
                                    .IsOk() &&
                                img.Format() == foundation::image::PixelFormat::RGBA8)
                            {
                                outW = img.Width();
                                outH = img.Height();
                                outPixels.Resize(img.PixelData().Size());
                                MemCopy(outPixels.Data(), img.PixelData().Data(),
                                        img.PixelData().Size());
                            }
                        }
                    }
                    else if (tex->embeddedWidth > 0 && tex->embeddedHeight > 0)
                    {
                        if (UniquePtr<IStream> stream = inst->ReadData(u8"pixels"))
                        {
                            const usize expected = static_cast<usize>(tex->embeddedWidth) *
                                                   tex->embeddedHeight * 4u;
                            if (static_cast<usize>(stream->Size()) == expected)
                            {
                                outPixels.Resize(expected);
                                if (stream->Read(outPixels.Data(), expected) == expected)
                                {
                                    outW = tex->embeddedWidth;
                                    outH = tex->embeddedHeight;
                                }
                                else
                                {
                                    outPixels.Clear();
                                }
                            }
                        }
                    }
                }
            }
        }

        // True iff at least one id is non-nil (a map array is built only on demand, R4).
        [[nodiscard]] static bool AnyNonNil(const Array<Guid>& ids)
        {
            for (usize i = 0; i < ids.Size(); ++i)
            {
                if (!ids[i].IsNil())
                {
                    return true;
                }
            }
            return false;
        }

        // Build one slice-major mip-chained RGBA8 array (header + texels) for `sliceCount` layers.
        // `ids[i]` (nil / out-of-range / undecodable) -> a slice filled with `defaultRGBA`. Mips:
        // the ALBEDO array (`srgb`) averages in LINEAR space (it uploads as RGBA8UnormSrgb -
        // averaging the encoded bytes darkens mips), the linear
        // normal/ORM arrays use the plain box filter.
        static void CookArray(pipeline::AssetBuildContext& ctx, const Array<Guid>& ids, u32 sliceCount,
                              u32 sliceSize, u32 mipCount, usize sliceBytes, const u8 defaultRGBA[4],
                              bool srgb, Array<u8>& blob)
        {
            const u32 header[3] = {sliceSize, mipCount, sliceCount};
            blob.Resize(sizeof(header) + sliceBytes * sliceCount);
            MemCopy(blob.Data(), header, sizeof(header));

            Array<u8> decoded;
            Array<u8> level;
            Array<u8> next;
            for (u32 slice = 0; slice < sliceCount; ++slice)
            {
                u32 w = 0, h = 0;
                const Guid id = (slice < ids.Size()) ? ids[slice] : Guid{};
                DecodeTextureRgba8(ctx, id, decoded, w, h);
                if (decoded.IsEmpty()) // nil / undecodable -> a 1x1 default slice
                {
                    w = 1;
                    h = 1;
                    decoded.Resize(4);
                    MemCopy(decoded.Data(), defaultRGBA, 4);
                }
                level.Resize(static_cast<usize>(sliceSize) * sliceSize * 4u);
                ResizeRgba8Bilinear(Span<const u8>{decoded.Data(), decoded.Size()}, w, h,
                                    Span<u8>{level.Data(), level.Size()}, sliceSize, sliceSize);
                u8* dst = blob.Data() + sizeof(header) + sliceBytes * slice;
                u32 dim = sliceSize;
                usize written = 0;
                for (u32 m = 0; m < mipCount; ++m)
                {
                    const usize bytes = static_cast<usize>(dim) * dim * 4u;
                    MemCopy(dst + written, level.Data(), bytes);
                    written += bytes;
                    if (m + 1 < mipCount)
                    {
                        const u32 half = dim > 1 ? dim / 2 : 1;
                        next.Resize(static_cast<usize>(half) * half * 4u);
                        if (srgb)
                        {
                            (void)BoxHalveRgba8SrgbAware(Span<const u8>{level.Data(), bytes}, dim,
                                                         Span<u8>{next.Data(), next.Size()});
                        }
                        else
                        {
                            (void)BoxHalveRgba8(Span<const u8>{level.Data(), bytes}, dim,
                                                Span<u8>{next.Data(), next.Size()});
                        }
                        level = next;
                        dim = half;
                    }
                }
            }
        }

        [[nodiscard]] static Status CookPaletteArray(const TerrainAsset& ta,
                                                     pipeline::AssetBuildContext& ctx)
        {
            if (ta.paletteAlbedoIds.IsEmpty())
            {
                // No palette (pure-base terrain): clear EVERY palette sidecar so a terrain whose
                // layers were all removed does not keep loading stale cooked arrays. Status
                // propagates like writeOrClear's delete path (a non-writable mount fails the cook
                // consistently, not just on the map-removal shape); a missing sidecar is Ok.
                const StringView streams[] = {
                    foundation::terrain::kPaletteStream,
                    foundation::terrain::kPaletteNormalStream,
                    foundation::terrain::kPaletteOrmStream,
                    foundation::terrain::kPaletteHeightStream,
                    foundation::terrain::kPaletteMaskStream,
                };
                for (const StringView stream : streams)
                {
                    const Status st = ctx.output->DeleteData(stream);
                    if (!st.IsOk())
                    {
                        return st;
                    }
                }
                return Status{};
            }
            // Snap the authored slice size to a sane power of two (mips need clean halving).
            u32 sliceSize = 64;
            while (sliceSize < static_cast<u32>(Max(ta.paletteTextureSize, 64)) &&
                   sliceSize < 4096u)
            {
                sliceSize *= 2;
            }
            u32 mipCount = 1;
            for (u32 d = sliceSize; d > 1; d /= 2)
            {
                ++mipCount;
            }
            const usize sliceBytes =
                foundation::terrain::TerrainPaletteData::SliceBytes(sliceSize, mipCount);
            const u32 sliceCount = static_cast<u32>(ta.paletteAlbedoIds.Size());

            // Per-map default slices: white albedo,
            // flat normal, default ORM, mid height (0.5 = neutral in the height-blend competition).
            static const u8 kWhite[4] = {255, 255, 255, 255};
            static const u8 kFlatNormal[4] = {128, 128, 255, 255};       // tangent-space +Z
            static const u8 kDefaultOrm[4] = {255, 255, 0, 255};         // AO 1, roughness 1, metallic 0
            static const u8 kMidHeight[4] = {128, 128, 128, 255};        // height 0.5 (.r used)
            static const u8 kOpaqueMask[4] = {255, 255, 255, 255};       // coverage 1 = fully opaque

            auto writeArray = [&](StringView stream, const Array<Guid>& ids, const u8 def[4],
                                  bool srgb) -> Status
            {
                Array<u8> blob;
                CookArray(ctx, ids, sliceCount, sliceSize, mipCount, sliceBytes, def, srgb, blob);
                return ctx.output->WriteData(
                    stream, Span<const byte>{reinterpret_cast<const byte*>(blob.Data()), blob.Size()});
            };

            // An optional map array is written when ANY layer supplies it, and OTHERWISE its sidecar
            // is DELETED - a re-cook that drops a map (the user cleared the picker) must not leave the
            // stale sidecar behind, or the factory keeps loading it (HasX() stays true). Idempotent.
            auto writeOrClear = [&](StringView stream, const Array<Guid>& ids, const u8 def[4],
                                    bool srgb) -> Status
            {
                if (AnyNonNil(ids))
                {
                    return writeArray(stream, ids, def, srgb);
                }
                return ctx.output->DeleteData(stream);
            };

            // Albedo is always present when the palette is non-empty; the rest are on demand (R4).
            if (Status s = writeArray(foundation::terrain::kPaletteStream, ta.paletteAlbedoIds, kWhite,
                                      /*srgb*/ true);
                !s.IsOk())
            {
                return s;
            }
            if (Status s = writeOrClear(foundation::terrain::kPaletteNormalStream, ta.paletteNormalIds,
                                        kFlatNormal, /*srgb*/ false);
                !s.IsOk())
            {
                return s;
            }
            if (Status s = writeOrClear(foundation::terrain::kPaletteOrmStream, ta.paletteOrmIds,
                                        kDefaultOrm, /*srgb*/ false);
                !s.IsOk())
            {
                return s;
            }
            if (Status s = writeOrClear(foundation::terrain::kPaletteHeightStream,
                                        ta.paletteHeightIds, kMidHeight, /*srgb*/ false);
                !s.IsOk())
            {
                return s;
            }
            if (Status s = writeOrClear(foundation::terrain::kPaletteMaskStream, ta.paletteMaskIds,
                                        kOpaqueMask, /*srgb*/ false);
                !s.IsOk())
            {
                return s;
            }
            return Status{};
        }
    };

    // Registers TerrainAsset for content-DB construction + deserialization (reflection body in
    // TerrainAssetImpl.cpp per GCC module hygiene).
    inline void RegisterTerrainAsset()
    {
        GlobalTypeRegistry().Register(TerrainAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<TerrainAsset>();
    }

    using foundation::terrain::SplatWeights;
    using foundation::terrain::SplatWeightsSource;

    // Source asset: the editable top-K splat weights of a given size. Authoring is CREATE + PAINT
    // (the TerrainPage seeds one, the Splat Paint tool writes it): the two rasters live in the
    // source instance's "pixels" (weights) + "indices" sidecars - the editable-source convention
    // (fileName empty = the sidecars are truth; both must exist once painted). fileName set = an
    // IMPORTED image, decoded as a fixed-layer raster (R = base share, G/B/A = palette 0..2) -
    // re-import explicitly resets any painted sidecars.
    class SplatmapAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(SplatmapAsset, pipeline::Asset)
    public:
        i32 width = 1024;  // raster resolution (authoring choice, independent of the heightfield)
        i32 height = 1024;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (import mode; empty = embedded/painted)
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
        }
    };

    // Cooks a SplatmapAsset -> SplatWeights resource (metadata object + the two streams).
    class SplatmapAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &SplatmapAsset::StaticType();
        }
        // Cooked instance type = the SERIALIZED SplatWeightsSource the cook stamps + ReadObject
        // rebuilds (SplatWeightsFactory.ProductType() is the runtime SplatWeights for Bind).
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SplatWeightsSource::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return 3; } // re-cook: top-K two-raster model

        // An EMBEDDED (create + paint) asset reads BOTH source streams - declare them so the
        // recipe hash chains their bytes (the envelope hash does not cover sidecars; a paint save
        // must re-cook). An IMPORTED one (fileName set) chains the file, which the base builder
        // already tracks.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const SplatmapAsset& sa = static_cast<const SplatmapAsset&>(asset);
            if (sa.fileName.IsEmpty())
            {
                out.sourceStreams.PushBack(String(foundation::terrain::kSplatStream));
                out.sourceStreams.PushBack(String(foundation::terrain::kSplatIndexStream));
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const SplatmapAsset& sa = static_cast<const SplatmapAsset&>(asset); // AssetType()-guarded
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            RefPtr<SplatWeights> sw;
            if (!sa.fileName.View().IsEmpty())
            {
                // IMPORTED: decode the image as RGBA8 at native size and convert it as a
                // fixed-layer raster (R = the base share, G/B/A = palette 0..2) - the only
                // meaningful interpretation of a flat image in the top-K model.
                Result<Array<byte>> bytes = ReadSourceBytes(ctx, sa.fileName.View());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                foundation::image::Image img;
                const Status loaded = foundation::image::io::LoadImageFromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                   bytes.Value().Size()),
                    img);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                if (img.Format() != foundation::image::PixelFormat::RGBA8)
                {
                    return Status{ErrorCode::NotSupported}; // HDR/other - weights are RGBA8
                }
                sw = foundation::terrain::SplatWeightsFromFixedLayerRaster(
                    img.PixelData(), static_cast<i32>(img.Width()),
                    static_cast<i32>(img.Height()), *ctx.allocator);
            }
            else
            {
                // EMBEDDED (create + paint): the rasters ride the source sidecars. Both present =
                // the top-K pair; neither = never painted (all-zero = pure base by construction,
                // no seeding). One without the other is a broken source - the cook fails.
                const i32 w = sa.width > 0 ? sa.width : 1;
                const i32 h = sa.height > 0 ? sa.height : 1;
                const usize expected = static_cast<usize>(w) * static_cast<usize>(h) *
                                       foundation::terrain::kSplatSlotCount;
                Array<u8> weights;
                Array<u8> indices;
                if (ctx.source != nullptr)
                {
                    weights = ReadStream(*ctx.source, foundation::terrain::kSplatStream);
                    indices = ReadStream(*ctx.source, foundation::terrain::kSplatIndexStream);
                }
                if (weights.Size() == expected && indices.Size() == expected)
                {
                    sw = MakeRef<SplatWeights>(*ctx.allocator, w, h);
                    MemCopy(sw->Weights().Data(), weights.Data(), expected);
                    MemCopy(sw->Indices().Data(), indices.Data(), expected);
                }
                else if (weights.IsEmpty() && indices.IsEmpty())
                {
                    sw = MakeRef<SplatWeights>(*ctx.allocator, w, h); // all base
                }
                else
                {
                    return Status{ErrorCode::NotSupported}; // one raster, or a size mismatch
                }
            }
            if (sw.Get() == nullptr || sw->IsEmpty())
            {
                return Status{ErrorCode::InvalidArgument};
            }

            SplatWeightsSource src;
            SplatWeightsSource::FromWeights(*sw, src);
            const Status wrote = ctx.output->WriteObject(src);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            const Status wroteWeights = ctx.output->WriteData(
                foundation::terrain::kSplatStream, SplatWeightsSource::WeightBlob(*sw));
            if (!wroteWeights.IsOk())
            {
                return wroteWeights;
            }
            return ctx.output->WriteData(foundation::terrain::kSplatIndexStream,
                                         SplatWeightsSource::IndexBlob(*sw));
        }

    private:
        [[nodiscard]] static Array<u8> ReadStream(content::Instance& instance, StringView name)
        {
            Array<u8> blob;
            if (UniquePtr<IStream> stream = instance.ReadData(name))
            {
                const i64 size = stream->Size();
                if (size > 0)
                {
                    blob.Resize(static_cast<usize>(size));
                    if (stream->Read(blob.Data(), static_cast<u64>(size)) !=
                        static_cast<u64>(size))
                    {
                        blob.Clear();
                    }
                }
            }
            return blob;
        }
    };

    // OS-file importer (editor drag-drop): imports a PNG (or any stb-decodable image) as a
    // SplatmapAsset with fileName set - the builder decodes it as a fixed-layer raster (R = base
    // share, G/B/A = palette 0..2) into the top-K model.
    class SplatmapFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Splatmap"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            return extension == u8"png";
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"SplatmapAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"SplatmapAsset");
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context, content::Group& group,
               const pipeline::ImportOptions* options, Object*, Array<pipeline::DeferredImportWrite>*) override
        {
            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, SplatmapAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();
            SplatmapAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    // Registers SplatmapAsset for content-DB construction + deserialization (reflection body in
    // TerrainAssetImpl.cpp per GCC module hygiene).
    inline void RegisterSplatmapAsset()
    {
        GlobalTypeRegistry().Register(SplatmapAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<SplatmapAsset>();
    }

    /// File > New's terrain creators (pipeline.registration composes every domain's).
    void RegisterTerrainCreators(AssetCreatorRegistry& registry);
}
