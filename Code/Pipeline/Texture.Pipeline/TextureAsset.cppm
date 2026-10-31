// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Texture - the `texture.pipeline` module (tooling).
//
// Source-side texture authoring and cook:
//   * TextureAsset (pipeline::Asset): references an image file and the GPU-texture
//     intent (color space, shape, sampler state), with presets for the common kinds.
//   * TextureAssetBuilder (DefaultAssetBuilder): cooks a TextureAsset into a
//     runtime TextureResource: decode the file, resolve the RHI format from the
//     pixel format and color space, write the cooked record and "data" pixel stream.
//   * TextureImporter: an authoring helper that produces a TextureAsset for an
//     image file with a sensible preset (2D / equirectangular sky).
//
// Never linked by the runtime. Cubemap import loads, validates and combines 6 face files; the
// builder cooks them into one cube (BuildCubemap, the +X face naming the other five).

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>

export module texture.pipeline;

import foundation.core;
import pipeline.core;
import pipeline.importer;
import foundation.rhi;
import foundation.texture;
import foundation.texture.resource;
import foundation.image;
import foundation.image.io;
import foundation.image.dds;
import foundation.content;
import texture.compression;

using namespace foundation::core;
using namespace foundation::texture;
namespace rhi = foundation::rhi;

export namespace pipeline{
    namespace image = foundation::image;
    namespace content = foundation::content;

    // Source asset: an image file and how it should become a GPU texture.
    class TextureAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(TextureAsset, pipeline::Asset)
    public:
        image::ImageColorSpace colorSpace = image::ImageColorSpace::Srgb;
        // Embedded mode (model imports): fileName empty and width/height set; the RGBA8 pixels
        // live in the source instance's "pixels" data stream instead of an external file.
        u32 embeddedWidth = 0;
        u32 embeddedHeight = 0;
        TextureShape shape = TextureShape::Texture2D;
        TextureFilter minFilter = TextureFilter::Linear;
        TextureFilter magFilter = TextureFilter::Linear;
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        TextureWrap wrapW = TextureWrap::Repeat;
        bool generateMipmaps = true;
        f32 anisotropy = 1.0f;
        // Block-compression authoring. Editor-data only: the builder feeds these
        // to the policy table to pick the cooked rhi::TextureFormat; the runtime never sees them.
        texcomp::TextureUsage usage = texcomp::TextureUsage::Color;
        texcomp::CompressionChoice compression = texcomp::CompressionChoice::Default;
        // Provenance, for display only: where an embedded fan-out texture came from (the model's
        // image uri, e.g. "textures/board_arm_4k.jpg"). It never drives loading: the embedded
        // or file branch keys on fileName/embeddedWidth, and this must stay out of it.
        String sourceHint;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "colorSpace", colorSpace);
            foundation::core::Serialize(ar, "usage", usage);
            foundation::core::Serialize(ar, "compression", compression);
            foundation::core::Serialize(ar, "embeddedWidth", embeddedWidth);
            foundation::core::Serialize(ar, "embeddedHeight", embeddedHeight);
            foundation::core::Serialize(ar, "shape", shape);
            foundation::core::Serialize(ar, "minFilter", minFilter);
            foundation::core::Serialize(ar, "magFilter", magFilter);
            foundation::core::Serialize(ar, "wrapU", wrapU);
            foundation::core::Serialize(ar, "wrapV", wrapV);
            foundation::core::Serialize(ar, "wrapW", wrapW);
            foundation::core::Serialize(ar, "generateMipmaps", generateMipmaps);
            foundation::core::Serialize(ar, "anisotropy", anisotropy);
            foundation::core::Serialize(ar, "sourceHint", sourceHint);
        }

        // Profiles (a sampler and the usage/colorSpace pair): each configures the whole set
        // coherently: sampler, shape, usage, and the color space the usage implies.
        // The page's profile buttons and the importer helpers share these one definitions.
        void SetupForUI()
        {
            usage = texcomp::TextureUsage::Color;
            colorSpace = image::ImageColorSpace::Srgb;
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::Linear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
        void SetupForSprite()
        {
            usage = texcomp::TextureUsage::Color;
            colorSpace = image::ImageColorSpace::Srgb;
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::Nearest;
            magFilter = TextureFilter::Nearest;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
        void SetupFor3D()
        {
            usage = texcomp::TextureUsage::Color;
            colorSpace = image::ImageColorSpace::Srgb;
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::MipmapLinear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::Repeat;
            wrapV = TextureWrap::Repeat;
            generateMipmaps = true;
            anisotropy = 16.0f;
        }
        void SetupForEquirectangularSkybox()
        {
            usage = texcomp::TextureUsage::HDR;
            colorSpace = image::ImageColorSpace::Linear;
            shape = TextureShape::Texture2D;
            minFilter = TextureFilter::Linear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            wrapW = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
        // A tangent-space normal map: the 3D sampler and the data semantics (linear, Normal policy).
        void SetupForNormalMap()
        {
            SetupFor3D();
            usage = texcomp::TextureUsage::Normal;
            colorSpace = image::ImageColorSpace::Linear;
        }
        // A data mask (roughness / AO / height / coverage): 3D sampler and linear single-channel policy.
        void SetupForDataMask()
        {
            SetupFor3D();
            usage = texcomp::TextureUsage::Mask;
            colorSpace = image::ImageColorSpace::Linear;
        }
        void SetupForCubemapSkybox()
        {
            usage = texcomp::TextureUsage::HDR;
            colorSpace = image::ImageColorSpace::Linear;
            shape = TextureShape::Cubemap;
            minFilter = TextureFilter::Linear;
            magFilter = TextureFilter::Linear;
            wrapU = TextureWrap::ClampToEdge;
            wrapV = TextureWrap::ClampToEdge;
            wrapW = TextureWrap::ClampToEdge;
            generateMipmaps = false;
            anisotropy = 1.0f;
        }
    };

    // Authoring helper: configure a TextureAsset for an image file with a preset.
    // (Asset is a non-copyable Object, so the result is filled in place.)
    class TextureImporter
    {
    public:
        // A standard 2D texture (3D preset: mips and anisotropy).
        static void Import2D(StringView path, image::ImageColorSpace colorSpace,
                             TextureAsset& outAsset)
        {
            outAsset.fileName = foundation::vfs::SourcePath(path);
            outAsset.SetupFor3D();
            outAsset.colorSpace = colorSpace;
        }

        // An HDR equirectangular sky (linear, clamped, no mips).
        static void ImportEquirectangular(StringView path, TextureAsset& outAsset)
        {
            outAsset.fileName = foundation::vfs::SourcePath(path);
            outAsset.SetupForEquirectangularSkybox();
        }

        // A cubemap sky from 6 face files (the first is stored as the asset source).
        static void ImportCubemap(StringView firstFacePath, TextureAsset& outAsset)
        {
            outAsset.fileName = foundation::vfs::SourcePath(firstFacePath);
            outAsset.SetupForCubemapSkybox();
        }

        // Load 6 cubemap faces into one combined buffer, faces concatenated in +X,-X,+Y,-Y,+Z,-Z order
        // (the layout a 6-layer cube texture expects). All faces must be square, the same size, and the
        // same format; the caller supplies the explicit paths. `outPixels` is 6 * faceSize*faceSize*bpp.
        [[nodiscard]] static Status LoadCubemap(Span<const StringView> facePaths,
                                                Array<u8>& outPixels, u32& outFaceSize)
        {
            if (facePaths.Size() != 6)
            {
                return ErrorCode::InvalidArgument;
            }
            image::Image faces[6];
            u32 faceSize = 0;
            usize faceBytes = 0;
            for (usize i = 0; i < 6; ++i)
            {
                if (!image::io::LoadImage(facePaths[i], faces[i]).IsOk())
                {
                    return ErrorCode::Unknown;
                }
                if (faces[i].Width() != faces[i].Height())
                {
                    return ErrorCode::Unknown;
                } // cube faces are square
                if (i == 0)
                {
                    faceSize = faces[0].Width();
                    faceBytes = faces[0].PixelData().Size();
                }
                else if (faces[i].Width() != faceSize || faces[i].PixelData().Size() != faceBytes ||
                         faces[i].Format() != faces[0].Format())
                {
                    return ErrorCode::Unknown;
                } // all faces must match
            }
            if (faceSize == 0 || faceBytes == 0)
            {
                return ErrorCode::Unknown;
            }
            outPixels.Resize(faceBytes * 6u);
            for (usize i = 0; i < 6; ++i)
            {
                MemCopy(outPixels.Data() + faceBytes * i, faces[i].PixelData().Data(), faceBytes);
            }
            outFaceSize = faceSize;
            return Status{};
        }

        // Given one face path (e.g. ".../sky_px.png"), derive all 6 face paths by matching a common
        // naming convention (px/nx/..., _posx/..., right/left/...) and rebuilding the set in
        // +X,-X,+Y,-Y,+Z,-Z order. Pure string derivation (no filesystem); pair with LoadCubemap, which
        // validates the files actually load. Returns Unknown if the path matches no known convention.
        [[nodiscard]] static Status DetectCubemapFaces(StringView oneFacePath,
                                                       Array<String>& outPaths)
        {
            const StringView dir = PathParent(oneFacePath);
            const StringView stem = PathStem(oneFacePath);
            const StringView ext = PathExtension(oneFacePath);
            static const StringView conv[5][6] = {
                {u8"px", u8"nx", u8"py", u8"ny", u8"pz", u8"nz"},
                {u8"_px", u8"_nx", u8"_py", u8"_ny", u8"_pz", u8"_nz"},
                {u8"_posx", u8"_negx", u8"_posy", u8"_negy", u8"_posz", u8"_negz"},
                {u8"_right", u8"_left", u8"_top", u8"_bottom", u8"_front", u8"_back"},
                {u8"right", u8"left", u8"top", u8"bottom", u8"front", u8"back"},
            };
            for (const auto& c : conv)
            {
                int matched = -1;
                for (int i = 0; i < 6; ++i)
                {
                    if (EndsWithCI(stem, c[i]))
                    {
                        matched = i;
                        break;
                    }
                }
                if (matched < 0)
                {
                    continue;
                }
                const StringView prefix = stem.SubStr(0, stem.Size() - c[matched].Size());
                outPaths.Clear();
                for (int i = 0; i < 6; ++i)
                {
                    String name{prefix};
                    name.Append(c[i]);
                    name.Append(ext);
                    outPaths.PushBack(dir.IsEmpty() ? name : PathJoin(dir, name.AsView()));
                }
                return Status{};
            }
            return ErrorCode::Unknown;
        }

    private:
        [[nodiscard]] static bool EndsWithCI(StringView s, StringView suffix) noexcept
        {
            if (s.Size() < suffix.Size())
            {
                return false;
            }
            const usize off = s.Size() - suffix.Size();
            for (usize i = 0; i < suffix.Size(); ++i)
            {
                utf8char a = s[off + i], b = suffix[i];
                if (a >= u8'A' && a <= u8'Z')
                {
                    a = static_cast<utf8char>(a + 32);
                }
                if (b >= u8'A' && b <= u8'Z')
                {
                    b = static_cast<utf8char>(b + 32);
                }
                if (a != b)
                {
                    return false;
                }
            }
            return true;
        }
    };

    // Cooks a TextureAsset to a TextureResource (decode and resolve the RHI format, then write the
    // cooked record and "data" pixel stream).
    class TextureAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        // v2: the "data" payload may carry a full mip chain (levels concatenated, mipLevels in
        // the record) instead of always level 0 only.
        // v3: the "data" payload may be block-compressed (BCn) when the asset's usage/compression
        // and target profile select it; resource.format then carries a BC format.
        // v4: Normal cooks BC7-linear, and the multichannel-Mask guard.
        // v5: HDR sources cook to BC6H on BC targets.
        [[nodiscard]] u32 Version() const override { return 5; }

        // Textures are the platform-variant producer: BC on desktop, ASTC on mobile-web from the same
        // source. The cook salts this builder's recipe by target and cooks per DB.
        [[nodiscard]] pipeline::BuildVariance Variance() const override
        {
            return pipeline::BuildVariance::PlatformVariant;
        }

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &TextureAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &TextureResource::StaticType();
        }

        // Embedded-mode textures read the "pixels" sidecar stream; declare it so the recipe
        // hash chains its bytes (the envelope hash doesn't cover sidecars).
        void ScanDependencies(const pipeline::Asset& asset,
                              pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const TextureAsset& ta = static_cast<const TextureAsset&>(asset);
            if (ta.fileName.IsEmpty() && ta.embeddedWidth > 0)
            {
                out.sourceStreams.PushBack(String(u8"pixels"));
            }
            // Cubemaps: fileName is the +X face (the implicit dep); the other 5 faces must
            // chain into the recipe hash too, or editing one never re-cooks the cube.
            if (ta.shape == TextureShape::Cubemap && !ta.fileName.IsEmpty())
            {
                Array<String> faces;
                if (TextureImporter::DetectCubemapFaces(ta.fileName.View(), faces).IsOk())
                {
                    for (usize i = 0; i < faces.Size(); ++i)
                    {
                        if (faces[i].AsView() != ta.fileName.View())
                        {
                            out.files.PushBack(foundation::vfs::SourcePath(faces[i].AsView()));
                        }
                    }
                }
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const TextureAsset& ta =
                static_cast<const TextureAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            // Embedded mode: the pixels stream is the decoded RGBA8 image (model imports).
            if (ta.fileName.IsEmpty() && ta.embeddedWidth > 0 && ta.embeddedHeight > 0)
            {
                return BuildEmbedded(ta, ctx);
            }

            // Cube-shaped assets load 6 face files (fileName is the +X face; the rest derive
            // from its naming convention) and cook them concatenated +X,-X,+Y,-Y,+Z,-Z.
            if (ta.shape == TextureShape::Cubemap)
            {
                return BuildCubemap(ta, ctx);
            }

            Result<Array<byte>> bytes = ReadSourceBytes(ctx, ta.fileName.View());
            if (!bytes.HasValue())
            {
                return Status{bytes.Error()};
            }
            const Span<const u8> raw(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                     bytes.Value().Size());
            // A DDS is GPU-ready already: its levels pass through when they fit, else level 0
            // decodes and cooks like any image (sniffed by magic, never by extension).
            if (image::dds::IsDds(raw))
            {
                return BuildDds(ta, ctx, raw);
            }
            image::Image image;
            const Stopwatch decodeClock = Stopwatch::StartNew();
            const Status loaded = image::io::LoadImageFromMemory(raw, image);
            if (!loaded.IsOk())
            {
                return loaded;
            }
            return BuildFromImage(ta, ctx, image,
                                  static_cast<i64>(decodeClock.Elapsed().AsMilliseconds()));
        }

    private:
        // The image path: a decoded 2D image, mips by the asset's flag, then the policy's format.
        [[nodiscard]] Status BuildFromImage(const TextureAsset& ta, pipeline::AssetBuildContext& ctx,
                                            const image::Image& image, i64 decodeMs = 0)
        {
            // The cook's own profile per texture (decode, mips, compress, write), so a slow
            // phase names itself in the console.
            const Stopwatch phaseClock = Stopwatch::StartNew();
            i64 mipsMs = 0;
            i64 compressMs = 0;
            TextureResource resource;
            resource.width = image.Width();
            resource.height = image.Height();
            resource.depthOrArrayLayers = 1;
            resource.mipLevels = 1;
            resource.format = TextureFormatUtils::Convert(image.Format(), ta.colorSpace);
            Array<byte> mipPixels; // level 0 (and the generated chain when asked); RGBA8 2D only
            {
                const Span<const u8> px0 = image.PixelData();
                mipPixels.Resize(px0.Size());
                if (px0.Size() != 0)
                {
                    MemCopy(mipPixels.Data(), px0.Data(), px0.Size());
                }
                if (ta.generateMipmaps && ta.shape == TextureShape::Texture2D &&
                    image.Format() == image::PixelFormat::RGBA8)
                {
                    resource.mipLevels = AppendMipChain(
                        mipPixels, image.Width(), image.Height(),
                        ta.colorSpace == image::ImageColorSpace::Srgb, ctx.jobs);
                }
                mipsMs = static_cast<i64>(phaseClock.Elapsed().AsMilliseconds());
                // Block-compress the RGBA8 chain when the policy table says to (2D RGBA8 only).
                if (ta.shape == TextureShape::Texture2D && image.Format() == image::PixelFormat::RGBA8)
                {
                    MaybeCompress(mipPixels, image.Width(), image.Height(), resource.mipLevels,
                                  ta.colorSpace == image::ImageColorSpace::Srgb, ta.usage,
                                  ta.compression, ProfileFor(ctx), resource.format, ctx.jobs);
                }
                // HDR (RGBA32F, one level: skies carry no mip chain) cooks to BC6H when the policy says.
                else if (ta.shape == TextureShape::Texture2D &&
                         image.Format() == image::PixelFormat::RGBA32F)
                {
                    MaybeCompressHdr(mipPixels, image.Width(), image.Height(), ta.usage,
                                     ta.compression, ProfileFor(ctx), resource.format);
                }
                compressMs = static_cast<i64>(phaseClock.Elapsed().AsMilliseconds()) - mipsMs;
            }
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;

            const i64 beforeWrite = static_cast<i64>(phaseClock.Elapsed().AsMilliseconds());
            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            const Status written = ctx.output->WriteData(
                u8"data", Span<const byte>(mipPixels.Data(), mipPixels.Size()));
            LOG_INFO(u8"Cook",
                     u8"texture {}x{}, {} level(s), format {}: decode {} ms, mips {} ms, "
                     u8"compress {} ms, write {} ms",
                     image.Width(), image.Height(), resource.mipLevels,
                     static_cast<i32>(resource.format), decodeMs, mipsMs, compressMs,
                     static_cast<i64>(phaseClock.Elapsed().AsMilliseconds()) - beforeWrite);
            return written;
        }

        // === DDS sources ========================================================================
        // A DDS carries GPU-ready levels (BC blocks, a mip chain). They pass through untouched
        // (lossless against the package, no encode) when the authored compression is not None,
        // the target reads BC, the block format fits the asset's usage by the policy table's own
        // rules (Colour: BC1/BC2/BC3/BC7; Normal: BC7 only, not BC5, until the shaders
        // reconstruct Z; Mask: BC4 or BC7; HDR: BC6H), and the file carries the mip chain the
        // asset asks for. Otherwise level 0 decodes and cooks as a plain image (mips generated,
        // format by policy): the decode-only fallback, also the ASTC / uncompressed route.
        [[nodiscard]] static rhi::TextureFormat DdsFormatToRhi(image::dds::DdsFormat format,
                                                                bool srgb)
        {
            using D = image::dds::DdsFormat;
            using F = rhi::TextureFormat;
            switch (image::dds::WithSrgb(format, srgb))
            {
            case D::BC1: return F::BC1RGBAUnorm;
            case D::BC1Srgb: return F::BC1RGBAUnormSrgb;
            case D::BC2: return F::BC2RGBAUnorm;
            case D::BC2Srgb: return F::BC2RGBAUnormSrgb;
            case D::BC3: return F::BC3RGBAUnorm;
            case D::BC3Srgb: return F::BC3RGBAUnormSrgb;
            case D::BC4: return F::BC4RUnorm;
            case D::BC4Snorm: return F::BC4RSnorm;
            case D::BC5: return F::BC5RGUnorm;
            case D::BC5Snorm: return F::BC5RGSnorm;
            case D::BC6HUf: return F::BC6HRGBUfloat;
            case D::BC6HSf: return F::BC6HRGBFloat;
            case D::BC7: return F::BC7RGBAUnorm;
            case D::BC7Srgb: return F::BC7RGBAUnormSrgb;
            default: return F::RGBA8Unorm; // uncompressed formats never pass through
            }
        }

        [[nodiscard]] static bool DdsFitsUsage(image::dds::DdsFormat format,
                                               texcomp::TextureUsage usage)
        {
            using D = image::dds::DdsFormat;
            const D f = image::dds::WithSrgb(format, false); // the twin is the asset's call
            switch (usage)
            {
            case texcomp::TextureUsage::Color:
                return f == D::BC1 || f == D::BC2 || f == D::BC3 || f == D::BC7;
            case texcomp::TextureUsage::Normal:
                return f == D::BC7;
            case texcomp::TextureUsage::Mask:
                return f == D::BC4 || f == D::BC7;
            case texcomp::TextureUsage::HDR:
                return f == D::BC6HUf || f == D::BC6HSf;
            }
            return false;
        }

        [[nodiscard]] static bool DdsPassesThrough(const TextureAsset& ta,
                                                   const image::dds::DdsImage& dds,
                                                   const texcomp::TargetProfile& profile)
        {
            if (ta.compression == texcomp::CompressionChoice::None || !profile.bc)
            {
                return false;
            }
            if (!image::dds::IsBlockCompressed(dds.format) || !DdsFitsUsage(dds.format, ta.usage))
            {
                return false;
            }
            // Mips wanted but not in the file: decode and generate them.
            const bool hasSize = dds.width > 1 || dds.height > 1;
            if (ta.generateMipmaps && dds.mipLevels < 2 && hasSize)
            {
                return false;
            }
            return true;
        }

        [[nodiscard]] Status BuildDds(const TextureAsset& ta, pipeline::AssetBuildContext& ctx,
                                      Span<const u8> raw)
        {
            image::dds::DdsImage dds;
            const Status loaded = image::dds::LoadDds(raw, dds);
            if (!loaded.IsOk())
            {
                LOG_ERROR(u8"Texture", u8"{}: DDS not readable ({})", ta.fileName.View(),
                          loaded.Code() == ErrorCode::NotSupported ? u8"a volume or a format outside the engine's set"
                                                                    : u8"truncated or malformed");
                return loaded;
            }
            if (dds.cubemap || dds.arrayLayers != 1 || ta.shape != TextureShape::Texture2D)
            {
                LOG_ERROR(u8"Texture", u8"{}: only 2D DDS sources cook (cubemap / array DDS: not yet)",
                          ta.fileName.View());
                return ErrorCode::NotSupported;
            }
            if (!DdsPassesThrough(ta, dds, ProfileFor(ctx)))
            {
                image::Image image;
                const Status decoded = image::dds::DecodeLevel(dds, 0, 0, image);
                if (!decoded.IsOk())
                {
                    return decoded;
                }
                return BuildFromImage(ta, ctx, image);
            }

            const u32 levels = ta.generateMipmaps ? dds.mipLevels : 1u; // no mips asked: level 0
            usize payload = 0;
            for (u32 level = 0; level < levels; ++level)
            {
                payload += dds.LevelSize(level);
            }
            TextureResource resource;
            resource.width = dds.width;
            resource.height = dds.height;
            resource.depthOrArrayLayers = 1;
            resource.mipLevels = levels;
            resource.format =
                DdsFormatToRhi(dds.format, ta.colorSpace == image::ImageColorSpace::Srgb);
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;
            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            return ctx.output->WriteData(
                u8"data", Span<const byte>(reinterpret_cast<const byte*>(dds.data.Data()), payload));
        }

        // === Block compression ==================================================================
        // The compressed-family profile for a cook: the target's capabilities, or
        // the always-BC desktop host when no target is set (the editor dev loop).
        [[nodiscard]] static texcomp::TargetProfile ProfileFor(const pipeline::AssetBuildContext& ctx)
        {
            if (ctx.target == nullptr)
            {
                return texcomp::DesktopProfile();
            }
            return texcomp::TargetProfile{ctx.target->bc, ctx.target->astc, ctx.target->etc2};
        }

        // The HDR counterpart: `pixels` holds ONE RGBA32F level (HDR sources cook without a mip
        // chain). When the policy resolves to BC6H the level is encoded in place and `format`
        // becomes BC6HRGBUfloat; otherwise both are left alone (None, small, no-BC target).
        static void MaybeCompressHdr(Array<byte>& pixels, u32 width, u32 height,
                                     texcomp::TextureUsage usage, texcomp::CompressionChoice choice,
                                     const texcomp::TargetProfile& profile, rhi::TextureFormat& format)
        {
            if (choice == texcomp::CompressionChoice::None)
            {
                return;
            }
            const rhi::TextureFormat chosen = texcomp::ResolveCompressedFormat(
                usage, /*sRGB*/ false, /*hasAlpha*/ false, /*multiChannel*/ true, choice, width,
                height, profile, format);
            if (chosen != rhi::TextureFormat::BC6HRGBUfloat)
            {
                return; // policy declined (or picked an LDR format for a mis-tagged usage)
            }
            const u8 quality = (choice == texcomp::CompressionChoice::Quality) ? 255u : 128u;
            Array<byte> out = texcomp::EncodeBlockCompressedHdr(
                reinterpret_cast<const f32*>(pixels.Data()), width, height, quality);
            if (out.Size() != rhi::CompressedLevelBytes(chosen, width, height))
            {
                return; // encoder refused (empty input): keep the uncompressed level
            }
            pixels = Move(out);
            format = chosen;
        }

        // Resolve the cooked format from the asset's authored usage/compression and the cook's
        // target profile, then, when the policy picks a compressed format, encode every RGBA8 mip
        // level in `pixels` to block bytes in place. `format` is updated to the chosen format
        // (unchanged when the policy declines: small, None, HDR, no supported family). `pixels`
        // holds the level-0..N-1 RGBA8 chain tightly concatenated; the compressed chain replaces it.
        static void MaybeCompress(Array<byte>& pixels, u32 width, u32 height, u32 mipLevels, bool srgb,
                                  texcomp::TextureUsage usage, texcomp::CompressionChoice choice,
                                  const texcomp::TargetProfile& profile, rhi::TextureFormat& format,
                                  JobSystem* jobs)
        {
            if (choice == texcomp::CompressionChoice::None)
            {
                return;
            }
            // hasAlpha splits Color into BC1 (opaque) and BC7 (alpha): scan level 0 for any non-opaque texel.
            bool hasAlpha = false;
            {
                const u8* p = reinterpret_cast<const u8*>(pixels.Data());
                const usize texels = static_cast<usize>(width) * height;
                for (usize i = 0; i < texels; ++i)
                {
                    if (p[i * 4 + 3] != 255)
                    {
                        hasAlpha = true;
                        break;
                    }
                }
            }
            // Channel sniff for the Mask guard: a packed ORM/ARM authored as Mask must
            // not cook channel-dropping BC4 (a gray rough/AO map still does).
            const bool multiChannel = texcomp::HasDistinctChannels(
                reinterpret_cast<const u8*>(pixels.Data()), width, height);
            const rhi::TextureFormat chosen = texcomp::ResolveCompressedFormat(
                usage, srgb, hasAlpha, multiChannel, choice, width, height, profile, format);
            if (!rhi::IsCompressed(chosen))
            {
                return; // policy declined: leave the RGBA8 chain and format as they are
            }
            const u8 quality = (choice == texcomp::CompressionChoice::Quality) ? 255u : 128u;

            Array<byte> out;
            usize srcOffset = 0;
            u32 w = width;
            u32 h = height;
            for (u32 level = 0; level < mipLevels; ++level)
            {
                const usize levelBytes = static_cast<usize>(w) * h * 4;
                const Array<byte> block = texcomp::EncodeBlockCompressed(
                    reinterpret_cast<const u8*>(pixels.Data() + srcOffset), w, h, chosen, quality,
                    jobs);
                if (block.Size() == 0)
                {
                    return; // encode failed: keep the uncompressed chain (safe fallback)
                }
                const usize at = out.Size();
                out.Resize(at + block.Size());
                MemCopy(out.Data() + at, block.Data(), block.Size());
                srcOffset += levelBytes;
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
            }
            pixels = Move(out);
            format = chosen;
        }

        // === Mip generation =====================================================================

        // sRGB <-> linear for the downsample: averaging must happen in linear space or mips
        // darken (a 50% black/white checker must average to linear 0.5 = sRGB ~188, not 128).
        [[nodiscard]] static f32 SrgbToLinearExact(u8 v)
        {
            const f32 c = static_cast<f32>(v) / 255.0f;
            return c <= 0.04045f ? c / 12.92f : Pow((c + 0.055f) / 1.055f, 2.4f);
        }
        [[nodiscard]] static u8 LinearToSrgbExact(f32 c)
        {
            c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
            const f32 encoded =
                c <= 0.0031308f ? c * 12.92f : 1.055f * Pow(c, 1.0f / 2.4f) - 0.055f;
            return static_cast<u8>(encoded * 255.0f + 0.5f);
        }
        // The mip filter's transfer functions as tables, built per chain (9 KB, ~8k Pow calls:
        // nothing next to a texture; a function-local static would be process-wide state in a
        // module interface, which a shared-library build cannot share). A 4k chain evaluates them
        // ~90 million times, and Pow per texel costs seconds of the cook. Decode is
        // exact (the input is 8-bit); encode quantizes linear to 1/8191 before the exact curve,
        // well under an sRGB code step everywhere but the first few codes, where it stays
        // within one.
        struct SrgbTables
        {
            f32 toLinear[256];
            u8 toSrgb[8192];
            SrgbTables()
            {
                for (u32 i = 0; i < 256; ++i)
                {
                    toLinear[i] = SrgbToLinearExact(static_cast<u8>(i));
                }
                for (u32 i = 0; i < 8192; ++i)
                {
                    toSrgb[i] = LinearToSrgbExact(static_cast<f32>(i) / 8191.0f);
                }
            }
        };


        /// Appends the full mip chain (levels 1..N, 2x2 box, clamped for odd dims) to
        /// `pixels`, which holds level 0 as tight RGBA8. Color channels of sRGB images
        /// filter in linear space; alpha (and everything in Linear images) averages
        /// directly. Returns the total level count including level 0.
        [[nodiscard]] static u32 AppendMipChain(Array<byte>& pixels, u32 width, u32 height,
                                                bool srgb, JobSystem* jobs = nullptr)
        {
            // Every destination row is independent: the big levels fan their rows out over the
            // cook's job system (a 4k chain takes seconds on one core in a Debug build); the
            // tables are hoisted out of the texel loop for the same reason.
            const SrgbTables tables;
            u32 levels = 1;
            usize srcOffset = 0;
            u32 srcW = width;
            u32 srcH = height;
            while (srcW > 1 || srcH > 1)
            {
                const u32 dstW = srcW > 1 ? srcW / 2 : 1;
                const u32 dstH = srcH > 1 ? srcH / 2 : 1;
                const usize dstOffset = pixels.Size();
                pixels.Resize(dstOffset + static_cast<usize>(dstW) * dstH * 4);
                const u8* src = reinterpret_cast<const u8*>(pixels.Data() + srcOffset);
                u8* dst = reinterpret_cast<u8*>(pixels.Data() + dstOffset);
                const auto filterRow = [&](u32 y)
                {
                    const u32 y0 = y * 2;
                    const u32 y1 = y0 + 1 < srcH ? y0 + 1 : y0; // clamp odd edges
                    for (u32 x = 0; x < dstW; ++x)
                    {
                        const u32 x0 = x * 2;
                        const u32 x1 = x0 + 1 < srcW ? x0 + 1 : x0;
                        const u8* p00 = src + (static_cast<usize>(y0) * srcW + x0) * 4;
                        const u8* p01 = src + (static_cast<usize>(y0) * srcW + x1) * 4;
                        const u8* p10 = src + (static_cast<usize>(y1) * srcW + x0) * 4;
                        const u8* p11 = src + (static_cast<usize>(y1) * srcW + x1) * 4;
                        u8* out = dst + (static_cast<usize>(y) * dstW + x) * 4;
                        for (i32 c = 0; c < 4; ++c)
                        {
                            if (srgb && c < 3) // color channels filter in linear space
                            {
                                const f32 avg =
                                    (tables.toLinear[p00[c]] + tables.toLinear[p01[c]] +
                                     tables.toLinear[p10[c]] + tables.toLinear[p11[c]]) *
                                    0.25f;
                                out[c] = tables.toSrgb[static_cast<u32>(avg * 8191.0f + 0.5f)];
                            }
                            else
                            {
                                out[c] = static_cast<u8>(
                                    (static_cast<u32>(p00[c]) + p01[c] + p10[c] + p11[c] + 2) / 4);
                            }
                        }
                    }
                };
                if (jobs != nullptr && dstH >= 64)
                {
                    jobs->ParallelFor(dstH, [&](u32 y) { filterRow(y); });
                }
                else
                {
                    for (u32 y = 0; y < dstH; ++y)
                    {
                        filterRow(y);
                    }
                }
                srcOffset = dstOffset;
                srcW = dstW;
                srcH = dstH;
                ++levels;
            }
            return levels;
        }

        // Cook a 6-face cubemap: derive the face paths from the +X face's naming convention,
        // load each through the VFS, validate (square, matching size/format), concatenate.
        [[nodiscard]] static Status BuildCubemap(const TextureAsset& ta,
                                                 pipeline::AssetBuildContext& ctx)
        {
            Array<String> facePaths;
            if (!TextureImporter::DetectCubemapFaces(ta.fileName.View(), facePaths).IsOk() ||
                facePaths.Size() != 6)
            {
                return Status{ErrorCode::InvalidArgument}; // fileName matches no face convention
            }

            image::Image faces[6];
            u32 faceSize = 0;
            usize faceBytes = 0;
            for (usize i = 0; i < 6; ++i)
            {
                Result<Array<byte>> bytes = ReadSourceBytes(ctx, facePaths[i].AsView());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                const Status loaded = image::io::LoadImageFromMemory(
                    Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                   bytes.Value().Size()),
                    faces[i]);
                if (!loaded.IsOk())
                {
                    return loaded;
                }
                if (faces[i].Width() != faces[i].Height())
                {
                    return Status{ErrorCode::InvalidArgument};
                }
                if (i == 0)
                {
                    faceSize = faces[0].Width();
                    faceBytes = faces[0].PixelData().Size();
                }
                else if (faces[i].Width() != faceSize || faces[i].PixelData().Size() != faceBytes ||
                         faces[i].Format() != faces[0].Format())
                {
                    return Status{ErrorCode::InvalidArgument}; // all faces must match
                }
            }
            if (faceSize == 0 || faceBytes == 0)
            {
                return Status{ErrorCode::InvalidArgument};
            }

            TextureResource resource;
            resource.width = faceSize;
            resource.height = faceSize;
            resource.depthOrArrayLayers = 6;
            resource.mipLevels = 1;
            resource.format = TextureFormatUtils::Convert(faces[0].Format(), ta.colorSpace);
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }

            Array<byte> pixels;
            pixels.Resize(faceBytes * 6u);
            for (usize i = 0; i < 6; ++i)
            {
                MemCopy(pixels.Data() + faceBytes * i, faces[i].PixelData().Data(), faceBytes);
            }
            return ctx.output->WriteData(u8"data", Span<const byte>(pixels.Data(), pixels.Size()));
        }

        [[nodiscard]] static Status BuildEmbedded(const TextureAsset& ta,
                                                  pipeline::AssetBuildContext& ctx)
        {
            if (ctx.source == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            UniquePtr<IStream> stream = ctx.source->ReadData(u8"pixels");
            if (stream.Get() == nullptr)
            {
                return Status{ErrorCode::NotFound};
            }
            const i64 size = stream->Size();
            const i64 expected = static_cast<i64>(ta.embeddedWidth) * ta.embeddedHeight * 4;
            if (size != expected)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            Array<byte> pixels;
            pixels.Resize(static_cast<usize>(size));
            if (stream->Read(pixels.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
            {
                return Status{ErrorCode::Unknown};
            }

            TextureResource resource;
            resource.width = ta.embeddedWidth;
            resource.height = ta.embeddedHeight;
            resource.depthOrArrayLayers = 1;
            resource.mipLevels = 1;
            resource.format = (ta.colorSpace == image::ImageColorSpace::Srgb)
                                  ? rhi::TextureFormat::RGBA8UnormSrgb
                                  : rhi::TextureFormat::RGBA8Unorm;
            if (ta.generateMipmaps && ta.shape == TextureShape::Texture2D)
            {
                resource.mipLevels =
                    AppendMipChain(pixels, ta.embeddedWidth, ta.embeddedHeight,
                                   ta.colorSpace == image::ImageColorSpace::Srgb);
            }
            if (ta.shape == TextureShape::Texture2D)
            {
                MaybeCompress(pixels, ta.embeddedWidth, ta.embeddedHeight, resource.mipLevels,
                              ta.colorSpace == image::ImageColorSpace::Srgb, ta.usage, ta.compression,
                              ProfileFor(ctx), resource.format, ctx.jobs);
            }
            resource.shape = ta.shape;
            resource.minFilter = ta.minFilter;
            resource.magFilter = ta.magFilter;
            resource.wrapU = ta.wrapU;
            resource.wrapV = ta.wrapV;
            resource.wrapW = ta.wrapW;
            resource.generateMipmaps = ta.generateMipmaps;
            resource.anisotropy = ta.anisotropy;

            const Status wrote = ctx.output->WriteObject(resource);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            return ctx.output->WriteData(u8"data", Span<const byte>(pixels.Data(), pixels.Size()));
        }
    };

    // OS-file importer (editor drag-drop): copies the image into Sources/ and creates a
    // TextureAsset instance named after the file stem in the target group. Preset by intent:
    //   - .hdr                                  -> equirectangular sky (linear/clamped/no mips)
    //   - a cube-face name (sky_px.png etc.) with all 6 sibling faces present
    //                                           -> one cube asset (all 6 faces copied)
    //   - anything else                         -> the standard 3D preset
    // Usage inference from texture-pack name tokens, case-insensitive, matched anywhere in
    // the stem. A token counts only when what follows it is the end, an underscore, or a digit
    // ("foo_nor_gl_4k" matches "_nor"; "my_armor" does not match "_arm": the boundary rule
    // keeps ordinary words out). Unrecognized names infer Color.
    [[nodiscard]] inline texcomp::TextureUsage InferTextureUsage(StringView stem)
    {
        String lower;
        lower.Reserve(stem.Size());
        for (usize i = 0; i < stem.Size(); ++i)
        {
            utf8char c = stem[i];
            if (c >= u8'A' && c <= u8'Z')
            {
                c = static_cast<utf8char>(c + 32);
            }
            lower.Append(c);
        }
        const auto hasToken = [&lower](StringView token) -> bool
        {
            const StringView haystack = lower.AsView();
            if (haystack.Size() < token.Size())
            {
                return false;
            }
            for (usize at = 0; at + token.Size() <= haystack.Size(); ++at)
            {
                bool match = true;
                for (usize i = 0; i < token.Size(); ++i)
                {
                    if (haystack[at + i] != token[i])
                    {
                        match = false;
                        break;
                    }
                }
                if (!match)
                {
                    continue;
                }
                const usize next = at + token.Size();
                if (next >= haystack.Size() || haystack[next] == u8'_' ||
                    (haystack[next] >= u8'0' && haystack[next] <= u8'9'))
                {
                    return true;
                }
            }
            return false;
        };
        // ambientCG glues the map's convention on ("Rock030_NormalGL", "_NormalDX"), which the
        // boundary rule would otherwise read as an ordinary word.
        for (StringView t : {u8"_normal", u8"_normalgl", u8"_normaldx", u8"_norm", u8"_nrm",
                             u8"_nor", u8"_ddn", u8"normalmap"})
        {
            if (hasToken(t))
            {
                return texcomp::TextureUsage::Normal;
            }
        }
        for (StringView t : {u8"_disp", u8"_height", u8"_mask", u8"_rough", u8"_ao", u8"_orm",
                             u8"_arm", u8"_metal"})
        {
            if (hasToken(t))
            {
                return texcomp::TextureUsage::Mask;
            }
        }
        return texcomp::TextureUsage::Color;
    }

    class TextureFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Texture"; }

        [[nodiscard]] bool Accepts(StringView extension) const override
        {
            for (StringView ext : {u8"png", u8"jpg", u8"jpeg", u8"tga", u8"bmp", u8"hdr", u8"dds"})
            {
                if (extension == ext)
                {
                    return true;
                }
            }
            return false;
        }

        // Usage inference from the universal texture-pack name tokens: the result is just the
        // stored fields; the page shows what was inferred and the author corrects it like any
        // edit. Unrecognized names keep the Color default.
        static void SetupForInferredUsage(TextureAsset& asset, StringView stem)
        {
            switch (InferTextureUsage(stem))
            {
            case texcomp::TextureUsage::Normal:
                asset.SetupForNormalMap();
                break;
            case texcomp::TextureUsage::Mask:
                asset.SetupForDataMask();
                break;
            default:
                asset.SetupFor3D();
                break;
            }
        }

        // A DDS names its own facts: BC5 is a normal map, BC4 a data mask, a float format an
        // HDR environment, and a DX10 header settles the colour space for a colour map. The
        // name tokens decide the rest. (An unreadable DDS imports like any file; the cook says why.)
        static void SetupForDds(TextureAsset& asset, StringView sourcePath, StringView stem)
        {
            // Only the header is read: it decides everything here, and reading whole files for a
            // model with hundreds of textures would be gigabytes on the editor's main thread.
            image::dds::DdsHeader dds;
            if (!image::dds::ReadDdsHeader(sourcePath, dds).IsOk())
            {
                SetupForInferredUsage(asset, stem);
                return;
            }
            using D = image::dds::DdsFormat;
            if (image::dds::IsHdr(dds.format))
            {
                asset.SetupForEquirectangularSkybox();
            }
            else if (dds.format == D::BC5 || dds.format == D::BC5Snorm)
            {
                asset.SetupForNormalMap();
            }
            else if (dds.format == D::BC4 || dds.format == D::BC4Snorm)
            {
                asset.SetupForDataMask();
            }
            else
            {
                SetupForInferredUsage(asset, stem);
            }
            if (dds.colorSpaceKnown && asset.usage == texcomp::TextureUsage::Color)
            {
                asset.colorSpace = image::dds::IsSrgb(dds.format) ? image::ImageColorSpace::Srgb
                                                                  : image::ImageColorSpace::Linear;
            }
        }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"TextureAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"TextureAsset");
        }

        [[nodiscard]] Result<content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               content::Group& group, const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            // Cubemap intent: the dropped file's stem matches a face convention (px/nx/...,
            // _posx/..., right/left/...) and all 6 sibling faces exist beside it. Any one
            // face can be dropped; the asset stores the +X face (the rest derive at cook).
            {
                Array<String> facePaths;
                if (TextureImporter::DetectCubemapFaces(sourcePath, facePaths).IsOk())
                {
                    bool allPresent = facePaths.Size() == 6;
                    for (const String& face : facePaths)
                    {
                        if (!FileExists(face.AsView()))
                        {
                            allPresent = false;
                            break;
                        }
                    }
                    if (allPresent)
                    {
                        return ImportCube(facePaths, context, group);
                    }
                }
            }

            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, TextureAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            content::Instance* instance = claimed.Value();
            const StringView stem = pipeline::FileStemOf(pipeline::FileNameOf(sourcePath));

            TextureAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            const String extension = pipeline::FileExtensionLower(sourcePath);
            if (extension == u8"hdr")
            {
                asset.SetupForEquirectangularSkybox(); // .hdr = an environment, not a surface map
            }
            else if (extension == u8"dds")
            {
                SetupForDds(asset, sourcePath, stem);
            }
            else
            {
                SetupForInferredUsage(asset, stem);
            }
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }

    private:
        // Copy all 6 faces into Sources/ and create one cube TextureAsset. fileName is the
        // +X face; the builder re-derives the face set from its naming convention at cook.
        [[nodiscard]] static Result<content::Instance*>
        ImportCube(const Array<String>& facePaths, const pipeline::ImportContext& context,
                   content::Group& group)
        {
            String posXName;
            for (usize i = 0; i < facePaths.Size(); ++i)
            {
                Result<String> copied =
                    pipeline::CopyIntoSources(context, facePaths[i].AsView());
                if (!copied.HasValue())
                {
                    return Err(copied.Error());
                }
                if (i == 0)
                {
                    posXName = copied.Value();
                }
            }

            // "sky_px" becomes "sky" (strip the face suffix and a trailing separator); fall back to
            // the full stem when the convention leaves nothing.
            const StringView posXStem = pipeline::FileStemOf(posXName.AsView());
            Array<String> derived;
            String name;
            if (TextureImporter::DetectCubemapFaces(posXName.AsView(), derived).IsOk())
            {
                // The convention suffix is whatever the +X path ends with beyond the shared prefix.
                usize common = 0;
                const StringView a = derived[0].AsView();
                const StringView b = derived[1].AsView();
                while (common < a.Size() && common < b.Size() && a[common] == b[common])
                {
                    ++common;
                }
                StringView prefix = pipeline::FileStemOf(a.SubStr(0, common));
                while (!prefix.IsEmpty() && (prefix[prefix.Size() - 1] == utf8char('_') ||
                                             prefix[prefix.Size() - 1] == utf8char('-')))
                {
                    prefix = prefix.SubStr(0, prefix.Size() - 1);
                }
                name = String(prefix);
            }
            if (name.IsEmpty())
            {
                name = String(posXStem);
            }

            content::Instance* instance =
                group.CreateInstance(name.AsView(), TextureAsset::StaticType());
            if (instance == nullptr)
            {
                return Err(ErrorCode::Unknown);
            }
            TextureAsset asset;
            asset.fileName = foundation::vfs::SourcePath(posXName.AsView());
            asset.SetupForCubemapSkybox();
            const Status written = instance->WriteObject(asset);
            if (!written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    /// What a render texture holds: Ldr is what a tonemapped view writes and what UI shows; Hdr
    /// keeps the linear values, for a material that wants them.
    enum class RenderTextureFormat : u32
    {
        Ldr = 0,
        Hdr = 1,
    };

    /// An authored render texture: a size and a format, and no source file. A camera's target
    /// draws into it; a sprite, a material or a UI image shows it.
    class RenderTextureAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(RenderTextureAsset, pipeline::Asset)
    public:
        u32 width = 256;
        u32 height = 256;
        RenderTextureFormat format = RenderTextureFormat::Ldr;

        /// Fill the cooked record this asset makes.
        void Cook(RenderTextureResource& cooked) const
        {
            cooked.width = width;
            cooked.height = height;
            cooked.format = (format == RenderTextureFormat::Hdr)
                                ? rhi::TextureFormat::RGBA16Float
                                : rhi::TextureFormat::RGBA8UnormSrgb;
        }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (unused: authored in-editor)
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
            foundation::core::Serialize(ar, "format", format);
        }
    };

    class RenderTextureAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &RenderTextureAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &RenderTextureResource::StaticType();
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            RenderTextureResource cooked;
            static_cast<const RenderTextureAsset&>(asset).Cook(cooked);
            if (!cooked.IsValid())
            {
                // A copy: the log takes its arguments by reference, and a reference to another
                // library's static constexpr member needs its address, which a shared build on
                // MSVC does not export (an unresolved external there).
                const u32 maxSize = kRenderTextureResourceMaxSize;
                LOG_ERROR(u8"Cook", u8"render texture {}x{}: each side must be 1 to {}",
                          cooked.width, cooked.height, maxSize);
                return Status{ErrorCode::InvalidArgument};
            }
            return ctx.output->WriteObject(cooked);
        }
    };

    /// File > New's texture creators (pipeline.registration composes every domain's).
    void RegisterTextureCreators(AssetCreatorRegistry& registry);
    /// RenderTextureFormat's names (the asset form's dropdown, the MCP envelopes' enum list).
    void RegisterRenderTextureReflection();

    // Registers TextureAsset for content-DB construction and deserialization. Also registers the
    // enum reflection its properties reference (owning modules; idempotent) so the generic asset
    // page can render enum-by-name dropdowns. TextureAsset's own reflection body (properties and
    // attributes) is TextureAsset::StaticType(), defined in TextureAssetImpl.cpp.
    inline void RegisterTextureAsset()
    {
        RegisterTextureReflection();          // TextureShape / TextureFilter / TextureWrap names
        image::RegisterImageReflection();     // ImageColorSpace names
        texcomp::RegisterCompressionReflection(); // TextureUsage / CompressionChoice names
        GlobalTypeRegistry().Register(TextureAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<TextureAsset>();
        RegisterTextureResource(); // the cooked records, RenderTextureResource among them
        RegisterRenderTextureReflection();
        GlobalTypeRegistry().Register(RenderTextureAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<RenderTextureAsset>();
    }
}
