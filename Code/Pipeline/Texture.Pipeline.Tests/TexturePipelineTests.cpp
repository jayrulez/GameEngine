// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Full GPU-texture asset pipeline: author a TextureAsset (image file and sampler
// intent), cook it with TextureAssetBuilder into an output content DB, then load the
// cooked TextureResource and build a live GPU Texture via the device-backed
// factory (Null RHI backend). Also exercises the TextureImporter authoring helper.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.rhi;
import foundation.rhi.null;
import pipeline.core;
import editor.core;
import foundation.xml.serialization;
import pipeline.importer;
import foundation.image;
import foundation.image.io;
import foundation.image.dds;
import foundation.texture;
import foundation.texture.resource;
import texture.pipeline;
import texture.compression;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::texture;
namespace image = foundation::image;
namespace rhi = foundation::rhi;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_texpipe_src.png");
        FileDelete(u8"scratch_texpipe_out_db/diffuse.rasset");
        FileDelete(u8"scratch_texpipe_out_db/diffuse.data.bin");
        RemoveDirectory(u8"scratch_texpipe_out_db");
    }
}

TEST_CASE("texture.pipeline: TextureAsset -> cook -> GPU Texture")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    RemoveTree();

    // A known 2x2 RGBA source image on disk.
    {
        image::Image src(2, 2, image::PixelFormat::RGBA8);
        Span<u8> px = src.PixelDataMut();
        for (usize i = 0; i < px.Size(); ++i)
        {
            px.Data()[i] = static_cast<u8>(i * 5);
        }
        REQUIRE(
            image::io::SaveImage(src, u8"scratch_texpipe_src.png", image::io::ImageFileFormat::PNG)
                .IsOk());
    }

    NativeFileSystem outMount(u8"scratch_texpipe_out_db", DefaultAllocator());
    Guid id;

    // --- cook (tooling): TextureAsset to TextureResource in the output DB ---
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"diffuse", TextureResource::StaticType());
        id = inst->Id();

        TextureAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"scratch_texpipe_src.png");
        asset.SetupForUI(); // clamp, no mips
        asset.colorSpace = image::ImageColorSpace::Srgb;

        TextureAssetBuilder builder;
        REQUIRE(builder.AssetType() == &TextureAsset::StaticType());
        foundation::vfs::NativeFileSystem srcMount(u8".", foundation::core::DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // --- runtime load: cooked TextureResource to a live GPU Texture ---
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    TextureFactory factory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&factory);

    Proxy<Texture> tex = manager.Bind<Texture>(id);
    REQUIRE(tex);
    CHECK(tex->Width() == 2u);
    CHECK(tex->Height() == 2u);
    CHECK(tex->Format() == rhi::TextureFormat::RGBA8UnormSrgb); // sRGB resolved from color space
    CHECK(tex->GpuTexture() != nullptr);
    CHECK(tex->Sampler() != nullptr);

    RemoveTree();
}

TEST_CASE("texture.importer: produces a TextureAsset with the right preset")
{
    TextureAsset a;
    TextureImporter::Import2D(u8"art/brick.png", image::ImageColorSpace::Srgb, a);
    CHECK(a.fileName == String(u8"art/brick.png"));
    CHECK(a.colorSpace == image::ImageColorSpace::Srgb);
    CHECK(a.generateMipmaps); // 3D preset
    CHECK(a.minFilter == TextureFilter::MipmapLinear);

    TextureAsset sky;
    TextureImporter::ImportEquirectangular(u8"sky/dusk.hdr", sky);
    CHECK(sky.colorSpace == image::ImageColorSpace::Linear);
    CHECK(sky.wrapU == TextureWrap::ClampToEdge);
    CHECK_FALSE(sky.generateMipmaps);
}

TEST_CASE("texture.pipeline: builder fails on a missing source file")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    RemoveTree();
    NativeFileSystem outMount(u8"scratch_texpipe_out_db", DefaultAllocator());
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    auto* inst = outDb.RootGroup()->CreateInstance(u8"diffuse", TextureResource::StaticType());

    TextureAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"does_not_exist_xyz.png");
    TextureAssetBuilder builder;
    foundation::vfs::NativeFileSystem srcMount(u8".", foundation::core::DefaultAllocator());
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &srcMount;
    ctx.output = inst;
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    RemoveTree();
}

TEST_CASE("texture-import: drag-dropped file becomes a Sources copy + TextureAsset instance")
{
    RegisterTextureAsset();

    const StringView dir = u8"scratch_tex_import_project";
    auto cleanTree = [&]()
    {
        FileDelete(PathJoin(dir, u8"Project.xml"));
        FileDelete(PathJoin(dir, u8"Sources/brick.png"));
        FileDelete(PathJoin(dir, u8"Content/brick.xasset"));
        for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
        {
            RemoveDirectory(PathJoin(dir, sub));
        }
        RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    // A loose "PNG" (the importer copies bytes and creates the asset; decoding happens at cook).
    const byte fakePng[6] = {byte{'P'}, byte{'N'}, byte{'G'}, byte{1}, byte{2}, byte{3}};
    REQUIRE(WriteFile(u8"brick.png", Span<const byte>(fakePng, 6)).IsOk());

    TextureFileImporter importer;
    CHECK(importer.Accepts(u8"png"));
    CHECK(importer.Accepts(u8"jpeg"));
    CHECK_FALSE(importer.Accepts(u8"gltf"));

    Result<foundation::content::Instance*> imported = importer.Import(
        u8"brick.png", pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()}, *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    foundation::content::Instance* instance = imported.Value();
    REQUIRE(instance != nullptr);
    CHECK(instance->Name() == StringView(u8"brick"));
    CHECK(instance->TypeName() == StringView(u8"TextureAsset"));

    // The source is in Sources/ and the asset references it by mount-relative name.
    CHECK(FileExists(PathJoin(dir, u8"Sources/brick.png").AsView()));
    RefPtr<ISerializable> object = instance->ReadObject();
    auto* asset = Cast<TextureAsset>(object.Get());
    REQUIRE(asset != nullptr);
    CHECK(asset->fileName == StringView(u8"brick.png"));
    CHECK(asset->generateMipmaps); // the 3D preset
    CHECK(asset->usage == texcomp::TextureUsage::Color); // plain name: Color default

    FileDelete(u8"brick.png");
    cleanTree();
}

TEST_CASE("texture-import: a normal-map suffix imports as Normal usage + linear color space")
{
    RegisterTextureAsset();

    const StringView dir = u8"scratch_tex_import_nrm_project";
    auto cleanTree = [&]()
    {
        FileDelete(PathJoin(dir, u8"Project.xml"));
        FileDelete(PathJoin(dir, u8"Sources/wall_normal.png"));
        for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
        {
            RemoveDirectory(PathJoin(dir, sub));
        }
        RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    const byte fakePng[6] = {byte{'P'}, byte{'N'}, byte{'G'}, byte{1}, byte{2}, byte{3}};
    REQUIRE(WriteFile(u8"wall_normal.png", Span<const byte>(fakePng, 6)).IsOk());

    TextureFileImporter importer;
    Result<foundation::content::Instance*> imported = importer.Import(
        u8"wall_normal.png", pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(imported.HasValue());
    RefPtr<ISerializable> object = imported.Value()->ReadObject();
    auto* asset = Cast<TextureAsset>(object.Get());
    REQUIRE(asset != nullptr);
    // The heuristic fired: a tangent normal cooks linear (BC7-linear policy), never sRGB color.
    CHECK(asset->usage == texcomp::TextureUsage::Normal);
    CHECK(asset->colorSpace == image::ImageColorSpace::Linear);

    FileDelete(u8"wall_normal.png");
    cleanTree();
}

TEST_CASE("texture.pipeline: cubemap - 6 faces cook into one cube product (end to end)")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    const StringView faceNames[6] = {
        u8"scratch_texpipe_sky_px.png", u8"scratch_texpipe_sky_nx.png",
        u8"scratch_texpipe_sky_py.png", u8"scratch_texpipe_sky_ny.png",
        u8"scratch_texpipe_sky_pz.png", u8"scratch_texpipe_sky_nz.png",
    };
    auto scrub = [&]()
    {
        for (StringView f : faceNames)
        {
            FileDelete(f);
        }
        FileDelete(u8"scratch_texpipe_cube_db/sky.rasset");
        FileDelete(u8"scratch_texpipe_cube_db/sky.data.bin");
        RemoveDirectory(u8"scratch_texpipe_cube_db");
    };
    scrub();

    // 6 distinct 4x4 faces (each filled with its face index so the concatenation order shows).
    for (u32 f = 0; f < 6; ++f)
    {
        image::Image src(4, 4, image::PixelFormat::RGBA8);
        Span<u8> px = src.PixelDataMut();
        for (usize i = 0; i < px.Size(); ++i)
        {
            px.Data()[i] = static_cast<u8>(f * 10 + 1);
        }
        REQUIRE(image::io::SaveImage(src, faceNames[f], image::io::ImageFileFormat::PNG).IsOk());
    }

    // The +X face's naming convention derives the whole set (what the builder does at cook).
    {
        Array<String> derived;
        REQUIRE(TextureImporter::DetectCubemapFaces(faceNames[0], derived).IsOk());
        REQUIRE(derived.Size() == 6u);
        CHECK(derived[5].AsView() == faceNames[5]);
    }

    NativeFileSystem outMount(u8"scratch_texpipe_cube_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), 
            outMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"sky", TextureResource::StaticType());
        id = inst->Id();

        TextureAsset asset;
        asset.fileName = foundation::vfs::SourcePath(faceNames[0]); // the +X face
        asset.SetupForCubemapSkybox();

        // The recipe must chain all faces (editing -nz alone must re-cook the cube).
        TextureAssetBuilder builder;
        pipeline::AssetBuildContext scanCtx{DefaultAllocator()};
        pipeline::AssetDependencies deps;
        builder.ScanDependencies(asset, scanCtx, deps);
        CHECK(deps.files.Size() == 5u); // the 5 non-+X faces (fileName is the implicit dep)

        foundation::vfs::NativeFileSystem srcMount(u8".", foundation::core::DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Runtime: the factory builds a real cube (6 layers, cube view, per-face upload).
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount, foundation::core::BinarySerializerFactory(),
                                             u8".rasset");
    TextureFactory factory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&factory);

    Proxy<Texture> tex = manager.Bind<Texture>(id);
    REQUIRE(tex);
    CHECK(tex->Width() == 4u);
    CHECK(tex->Height() == 4u);
    CHECK(tex->IsCube());
    CHECK(tex->Uid() != 0u);
    CHECK(tex->GpuTexture() != nullptr);

    scrub();
}

TEST_CASE("texture.pipeline: mip chain cook - counts, sizes, and sRGB-correct averaging")
{
    // The cook builds the mip chain. Pins:
    // (1) chain shape: a 4x4 cooks 3 levels, payload = (16+4+1)*4 bytes;
    // (2) sRGB correctness: a black/white checker's mip must average in linear space
    //     (sRGB ~188), not the naive byte average 127;
    // (3) Linear images average bytes directly (a data map's mip stays 127/128);
    // (4) odd dims clamp (5x3, then 2x1, then 1x1).
    RegisterTextureResource();
    RegisterTextureAsset();

    const auto cookEmbedded = [](u32 w, u32 h, image::ImageColorSpace cs,
                                 Function<u8(u32, u32, i32)> pixelAt, StringView dbDir,
                                 u32& outMipLevels, Array<u8>& outPayload) {
        (void)RemoveDirectoryRecursive(dbDir);
        NativeFileSystem srcMount(dbDir, DefaultAllocator());
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            srcMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* srcInst = db.RootGroup()->CreateInstance(u8"src", TextureAsset::StaticType());
        auto* outInst = db.RootGroup()->CreateInstance(u8"out", TextureResource::StaticType());

        Array<byte> pixels;
        pixels.Resize(static_cast<usize>(w) * h * 4);
        for (u32 y = 0; y < h; ++y)
        {
            for (u32 x = 0; x < w; ++x)
            {
                for (i32 c = 0; c < 4; ++c)
                {
                    pixels[(static_cast<usize>(y) * w + x) * 4 + static_cast<usize>(c)] =
                        static_cast<byte>(pixelAt(x, y, c));
                }
            }
        }
        TextureAsset asset;
        asset.embeddedWidth = w;
        asset.embeddedHeight = h;
        asset.colorSpace = cs;
        asset.generateMipmaps = true;
        REQUIRE(srcInst->WriteObject(asset).IsOk());
        REQUIRE(srcInst->WriteData(u8"pixels", Span<const byte>(pixels.Data(), pixels.Size()))
                    .IsOk());

        TextureAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.source = srcInst;
        ctx.output = outInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        RefPtr<ISerializable> object = outInst->ReadObject();
        auto* res = Cast<TextureResource>(object.Get());
        REQUIRE(res != nullptr);
        outMipLevels = res->mipLevels;
        UniquePtr<IStream> data = outInst->ReadData(u8"data");
        REQUIRE(data);
        outPayload.Resize(static_cast<usize>(data->Size()));
        REQUIRE(data->Read(outPayload.Data(), static_cast<u64>(outPayload.Size())) ==
                static_cast<u64>(outPayload.Size()));
        (void)RemoveDirectoryRecursive(dbDir);
    };

    // (1) and (2): 4x4 sRGB checkerboard (0 / 255 alternating).
    {
        u32 mipLevels = 0;
        Array<u8> payload;
        cookEmbedded(4, 4, image::ImageColorSpace::Srgb,
                     Function<u8(u32, u32, i32)>{[](u32 x, u32 y, i32 c)
                                                 { return c == 3 ? u8{255}
                                                                 : (((x + y) & 1) ? u8{255}
                                                                                  : u8{0}); }},
                     u8"scratch_texpipe_mips_srgb", mipLevels, payload);
        CHECK(mipLevels == 3u);
        CHECK(payload.Size() == (16u + 4u + 1u) * 4u);
        // mip1 first texel: 2x2 checker averages to linear 0.5, sRGB ~188 (not 127).
        const u8 mip1 = payload[16u * 4u];
        CHECK(mip1 >= 186);
        CHECK(mip1 <= 190);
        // Alpha averages directly (all 255).
        CHECK(payload[16u * 4u + 3u] == 255);
    }

    // (3): the same checker as a Linear data map keeps the straight byte average.
    {
        u32 mipLevels = 0;
        Array<u8> payload;
        cookEmbedded(4, 4, image::ImageColorSpace::Linear,
                     Function<u8(u32, u32, i32)>{[](u32 x, u32 y, i32 c)
                                                 { return c == 3 ? u8{255}
                                                                 : (((x + y) & 1) ? u8{255}
                                                                                  : u8{0}); }},
                     u8"scratch_texpipe_mips_linear", mipLevels, payload);
        CHECK(mipLevels == 3u);
        const u8 mip1 = payload[16u * 4u];
        CHECK(mip1 >= 127);
        CHECK(mip1 <= 128);
    }

    // (4): odd dims clamp down the chain: 5x3, 2x1, 1x1.
    {
        u32 mipLevels = 0;
        Array<u8> payload;
        cookEmbedded(5, 3, image::ImageColorSpace::Linear,
                     Function<u8(u32, u32, i32)>{[](u32, u32, i32) { return u8{200}; }},
                     u8"scratch_texpipe_mips_npot", mipLevels, payload);
        CHECK(mipLevels == 3u);
        CHECK(payload.Size() == (15u + 2u + 1u) * 4u);
    }
}

TEST_CASE("texture.pipeline: block compression cook - format policy + exact cooked-DB size drop")
{
    // The cook block-compresses the RGBA8 mip chain when the asset's authored
    // usage/compression and the desktop (BC) profile select a BC format. Pins:
    // (1) usage/compression picks the cooked resource.format; (2) the "data" payload is exactly the
    // sum of per-level block bytes; (3) the size drop against uncompressed is real (BC1 ~8x, BC7 ~4x).
    RegisterTextureResource();
    RegisterTextureAsset();

    // Cook a 128x128 embedded image (>64px so it clears the small-texture escape hatch) with the
    // given authoring, returning the cooked format and "data" payload size.
    const auto cook = [](texcomp::TextureUsage usage, texcomp::CompressionChoice choice,
                         image::ImageColorSpace cs, bool alpha, StringView dbDir,
                         rhi::TextureFormat& outFormat, u32& outMips, usize& outPayload,
                         bool gray = false) {
        const u32 w = 128, h = 128;
        (void)RemoveDirectoryRecursive(dbDir);
        NativeFileSystem srcMount(dbDir, DefaultAllocator());
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), 
            srcMount, foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* srcInst = db.RootGroup()->CreateInstance(u8"src", TextureAsset::StaticType());
        auto* outInst = db.RootGroup()->CreateInstance(u8"out", TextureResource::StaticType());

        Array<byte> pixels;
        pixels.Resize(static_cast<usize>(w) * h * 4);
        for (u32 y = 0; y < h; ++y)
        {
            for (u32 x = 0; x < w; ++x)
            {
                byte* p = pixels.Data() + (static_cast<usize>(y) * w + x) * 4;
                p[0] = static_cast<byte>((x * 255) / (w - 1));
                p[1] = gray ? p[0] : static_cast<byte>((y * 255) / (h - 1));
                p[2] = gray ? p[0] : static_cast<byte>(((x + y) * 255) / (w + h - 2));
                p[3] = alpha ? static_cast<byte>((x * 255) / (w - 1)) : static_cast<byte>(255);
            }
        }
        TextureAsset asset;
        asset.embeddedWidth = w;
        asset.embeddedHeight = h;
        asset.colorSpace = cs;
        asset.generateMipmaps = true;
        asset.usage = usage;
        asset.compression = choice;
        REQUIRE(srcInst->WriteObject(asset).IsOk());
        REQUIRE(srcInst->WriteData(u8"pixels", Span<const byte>(pixels.Data(), pixels.Size())).IsOk());

        TextureAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.source = srcInst;
        ctx.output = outInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());

        RefPtr<ISerializable> object = outInst->ReadObject();
        auto* res = Cast<TextureResource>(object.Get());
        REQUIRE(res != nullptr);
        outFormat = res->format;
        outMips = res->mipLevels;
        UniquePtr<IStream> data = outInst->ReadData(u8"data");
        REQUIRE(data);
        outPayload = static_cast<usize>(data->Size());
        (void)RemoveDirectoryRecursive(dbDir);
    };

    // Sum of per-level block bytes for a full 128x128 chain of `format` (the exact cooked size).
    const auto expectedCompressed = [](rhi::TextureFormat format, u32 levels) {
        usize total = 0;
        u32 w = 128, h = 128;
        for (u32 i = 0; i < levels; ++i)
        {
            total += rhi::CompressedLevelBytes(format, w, h);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        return total;
    };

    rhi::TextureFormat fmt{};
    u32 mips = 0;
    usize payload = 0;

    // (1) Color, opaque, sRGB, Default: BC1 sRGB; the payload is the exact BC1 block bytes.
    cook(texcomp::TextureUsage::Color, texcomp::CompressionChoice::Default,
         image::ImageColorSpace::Srgb, /*alpha*/ false, u8"scratch_texpipe_bc1", fmt, mips, payload);
    CHECK(fmt == rhi::TextureFormat::BC1RGBAUnormSrgb);
    CHECK(payload == expectedCompressed(rhi::TextureFormat::BC1RGBAUnormSrgb, mips));

    // The uncompressed RGBA8 chain, for the size-drop comparison.
    usize rawPayload = 0;
    cook(texcomp::TextureUsage::Color, texcomp::CompressionChoice::None, image::ImageColorSpace::Srgb,
         false, u8"scratch_texpipe_raw", fmt, mips, rawPayload);
    CHECK(fmt == rhi::TextureFormat::RGBA8UnormSrgb);
    // BC1 is 4 bits/texel against 32, so the cooked payload is ~8x smaller. Guard a real, large drop.
    CHECK(payload * 6u < rawPayload);

    // (2) Color with alpha: BC7; Quality on an opaque image also forces BC7.
    cook(texcomp::TextureUsage::Color, texcomp::CompressionChoice::Default,
         image::ImageColorSpace::Srgb, /*alpha*/ true, u8"scratch_texpipe_bc7a", fmt, mips, payload);
    CHECK(fmt == rhi::TextureFormat::BC7RGBAUnormSrgb);
    CHECK(payload == expectedCompressed(rhi::TextureFormat::BC7RGBAUnormSrgb, mips));

    cook(texcomp::TextureUsage::Color, texcomp::CompressionChoice::Quality,
         image::ImageColorSpace::Linear, false, u8"scratch_texpipe_bc7q", fmt, mips, payload);
    CHECK(fmt == rhi::TextureFormat::BC7RGBAUnorm);

    // (3) Normal: BC7-linear (never BC5: the shaders decode rgb * 2 - 1 and BC5's
    // missing B collapses tangent z to -1, shading the normals white).
    cook(texcomp::TextureUsage::Normal, texcomp::CompressionChoice::Default,
         image::ImageColorSpace::Linear, false, u8"scratch_texpipe_bc7n", fmt, mips, payload);
    CHECK(fmt == rhi::TextureFormat::BC7RGBAUnorm);
    CHECK(payload == expectedCompressed(rhi::TextureFormat::BC7RGBAUnorm, mips));

    // Mask with distinct channel content (this fixture's gradient has a packed ORM shape):
    // the channel sniff routes it to BC7-linear, since BC4 would silently drop G/B.
    cook(texcomp::TextureUsage::Mask, texcomp::CompressionChoice::Default,
         image::ImageColorSpace::Linear, false, u8"scratch_texpipe_maskorm", fmt, mips, payload);
    CHECK(fmt == rhi::TextureFormat::BC7RGBAUnorm);

    // A true single-channel mask (gray: R=G=B) keeps BC4.
    cook(texcomp::TextureUsage::Mask, texcomp::CompressionChoice::Default,
         image::ImageColorSpace::Linear, false, u8"scratch_texpipe_bc4", fmt, mips, payload,
         /*gray*/ true);
    CHECK(fmt == rhi::TextureFormat::BC4RUnorm);
    CHECK(payload == expectedCompressed(rhi::TextureFormat::BC4RUnorm, mips));
}

TEST_CASE("texture.asset: pre-variants payloads (no usage/compression keys) still deserialize")
{
    // The XML serializer is strict (a missing key fails the whole payload), so the usage and
    // compression fields are v2-gated: read unconditionally, they would break ReadObject for every
    // older texture envelope. A v0 payload (the old files' shape for these fields) must parse and
    // keep the defaults; a current versioned round-trip must keep authored values.
    using foundation::xml::XmlSerializerFactory;

    // Old-shape payload: serialize without a version scope (Version() == 0 skips the v2
    // fields on write, producing the older key set).
    foundation::core::MemoryStream oldBytes;
    {
        auto ctx = XmlSerializerFactory()(oldBytes, foundation::core::SerializeMode::Write);
        REQUIRE(ctx.Get() != nullptr);
        pipeline::TextureAsset asset;
        asset.generateMipmaps = true; // a non-default v0 field must survive
        asset.Serialize(*ctx->serializer);
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(oldBytes);
    }
    {
        (void)oldBytes.Seek(0, foundation::core::SeekOrigin::Begin);
        auto ctx = XmlSerializerFactory()(oldBytes, foundation::core::SerializeMode::Read);
        REQUIRE(ctx.Get() != nullptr);
        pipeline::TextureAsset asset;
        asset.usage = texcomp::TextureUsage::Normal; // a v0 read skips these fields entirely,
        asset.usage = texcomp::TextureUsage::Color;  // so the defaults stay put
        asset.Serialize(*ctx->serializer);
        CHECK(ctx->serializer->IsOk()); // an ungated read fails here with NotFound
        CHECK(asset.generateMipmaps == true);
        CHECK(asset.usage == texcomp::TextureUsage::Color);
        CHECK(asset.compression == texcomp::CompressionChoice::Default);
    }

    // Current-format round-trip: the v2 scope writes and reads the new fields.
    foundation::core::MemoryStream newBytes;
    {
        auto ctx = XmlSerializerFactory()(newBytes, foundation::core::SerializeMode::Write);
        pipeline::TextureAsset asset;
        asset.usage = texcomp::TextureUsage::Normal;
        asset.compression = texcomp::CompressionChoice::Quality;
        foundation::core::BeginVersionedPayload(*ctx->serializer,
                                                pipeline::TextureAsset::StaticType());
        asset.Serialize(*ctx->serializer);
        foundation::core::EndVersionedPayload(*ctx->serializer);
        REQUIRE(ctx->serializer->IsOk());
        ctx->Flush(newBytes);
    }
    {
        (void)newBytes.Seek(0, foundation::core::SeekOrigin::Begin);
        auto ctx = XmlSerializerFactory()(newBytes, foundation::core::SerializeMode::Read);
        pipeline::TextureAsset asset;
        foundation::core::BeginVersionedPayload(*ctx->serializer,
                                                pipeline::TextureAsset::StaticType());
        asset.Serialize(*ctx->serializer);
        foundation::core::EndVersionedPayload(*ctx->serializer);
        CHECK(ctx->serializer->IsOk());
        CHECK(asset.usage == texcomp::TextureUsage::Normal);
        CHECK(asset.compression == texcomp::CompressionChoice::Quality);
    }
}

TEST_CASE("texture import: usage inference from texture-pack name tokens")
{
    using pipeline::InferTextureUsage;
    // The universal suffixes, matched mid-stem with the boundary rule.
    CHECK(InferTextureUsage(u8"foo_nor_gl_4k") == texcomp::TextureUsage::Normal);
    CHECK(InferTextureUsage(u8"brick_NORMAL") == texcomp::TextureUsage::Normal);
    CHECK(InferTextureUsage(u8"Rock030_NormalGL") == texcomp::TextureUsage::Normal); // ambientCG
    CHECK(InferTextureUsage(u8"Rock030_1K-JPG_NormalDX") == texcomp::TextureUsage::Normal);
    CHECK(InferTextureUsage(u8"bar_disp_2k") == texcomp::TextureUsage::Mask);
    CHECK(InferTextureUsage(u8"ground_rough_1k") == texcomp::TextureUsage::Mask);
    CHECK(InferTextureUsage(u8"kit_orm") == texcomp::TextureUsage::Mask);
    CHECK(InferTextureUsage(u8"trim_AO_2k") == texcomp::TextureUsage::Mask);
    // Everything else keeps the Color default, including boundary-rule near-misses.
    CHECK(InferTextureUsage(u8"grass_diff") == texcomp::TextureUsage::Color);
    CHECK(InferTextureUsage(u8"my_armor") == texcomp::TextureUsage::Color);   // "_arm" then a letter
    CHECK(InferTextureUsage(u8"town_north") == texcomp::TextureUsage::Color); // "_nor" then a letter
}

TEST_CASE("texture profiles configure usage + color space with the sampler")
{
    pipeline::TextureAsset a;
    a.SetupForNormalMap();
    CHECK(a.usage == texcomp::TextureUsage::Normal);
    CHECK(a.colorSpace == image::ImageColorSpace::Linear);
    CHECK(a.generateMipmaps);                                  // rides the 3D sampler
    CHECK(a.wrapU == TextureWrap::Repeat);

    a.SetupForDataMask();
    CHECK(a.usage == texcomp::TextureUsage::Mask);
    CHECK(a.colorSpace == image::ImageColorSpace::Linear);

    a.SetupForUI();
    CHECK(a.usage == texcomp::TextureUsage::Color);
    CHECK(a.colorSpace == image::ImageColorSpace::Srgb);
    CHECK(!a.generateMipmaps);

    a.SetupForEquirectangularSkybox();
    CHECK(a.usage == texcomp::TextureUsage::HDR);
    CHECK(a.colorSpace == image::ImageColorSpace::Linear);

    a.SetupForCubemapSkybox();
    CHECK(a.usage == texcomp::TextureUsage::HDR);
    CHECK(a.colorSpace == image::ImageColorSpace::Linear);
    CHECK(a.shape == TextureShape::Cubemap);
}

TEST_CASE("texture-import: DescribeImport lists one asset; a selection rename renames it")
{
    RegisterTextureAsset();

    const StringView dir = u8"scratch_tex_rename_project";
    auto cleanTree = [&]()
    {
        FileDelete(PathJoin(dir, u8"Project.xml"));
        FileDelete(PathJoin(dir, u8"Sources/wall.png"));
        FileDelete(PathJoin(dir, u8"Content/wall_albedo.xasset"));
        for (StringView sub : {u8"Content", u8"Sources", u8"Cooked", u8"Editor", u8".cache"})
        {
            RemoveDirectory(PathJoin(dir, sub));
        }
        RemoveDirectory(dir);
    };
    cleanTree();
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));

    const byte fakePng[6] = {byte{'P'}, byte{'N'}, byte{'G'}, byte{4}, byte{5}, byte{6}};
    REQUIRE(WriteFile(u8"wall.png", Span<const byte>(fakePng, 6)).IsOk());

    TextureFileImporter importer;
    pipeline::ImportPlan plan = importer.DescribeImport(u8"wall.png", nullptr, nullptr);
    REQUIRE(plan.entries.Size() == 1u);
    CHECK(plan.entries[0].kind == pipeline::ImportResourceKind::Asset);
    CHECK(plan.entries[0].sourceName == u8"wall");
    CHECK(plan.entries[0].targetName == u8"wall");

    // The review dialog's rename travels on the base options (option-less importer).
    auto options = MakeRef<pipeline::ImportOptions>(DefaultAllocator());
    plan.entries[0].targetName = String(u8"wall_albedo");
    options->selection = Move(plan);
    Result<foundation::content::Instance*> imported = importer.Import(
        u8"wall.png", pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), options.Get(), nullptr, nullptr);
    REQUIRE(imported.HasValue());
    CHECK(imported.Value()->Name() == StringView(u8"wall_albedo"));
    CHECK(project->SourceDb().RootGroup()->GetInstance(u8"wall") == nullptr);
    // The source keeps its file identity; only the asset instance is renamed.
    CHECK(FileExists(PathJoin(dir, u8"Sources/wall.png").AsView()));

    // Re-import memory: the renamed asset is found again through its typed fileName
    // back-reference; merging pre-seeds the fresh plan with the rename, and committing
    // with it updates the renamed instance instead of minting a duplicate "wall".
    pipeline::ImportPlan stored =
        importer.StoredSelection(*project->SourceDb().RootGroup(), u8"wall.png");
    REQUIRE(stored.entries.Size() == 1u);
    CHECK(stored.entries[0].targetName == u8"wall_albedo");
    pipeline::ImportPlan fresh = importer.DescribeImport(u8"wall.png", nullptr, nullptr);
    pipeline::MergeStoredSelection(fresh, stored);
    CHECK(fresh.entries[0].targetName == u8"wall_albedo");
    auto reOptions = MakeRef<pipeline::ImportOptions>(DefaultAllocator());
    reOptions->selection = Move(fresh);
    Result<foundation::content::Instance*> reimported = importer.Import(
        u8"wall.png", pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
        *project->SourceDb().RootGroup(), reOptions.Get(), nullptr, nullptr);
    REQUIRE(reimported.HasValue());
    CHECK(reimported.Value() == imported.Value()); // same instance, updated
    CHECK(project->SourceDb().RootGroup()->GetInstance(u8"wall") == nullptr);

    FileDelete(u8"wall.png");
    cleanTree();
}

namespace
{
    // A flat (non-RLE) Radiance .hdr: a header and one RGBE quad per pixel. stb_image reads the flat
    // layout whenever the first pixel is not the RLE marker (2, 2, hi, lo), which a real radiance
    // value never is.
    void WriteFlatRadianceHdr(const char* path, u32 w, u32 h, f32 r, f32 g, f32 b)
    {
        // Built in memory and written through the engine's own file write (std::fopen is a
        // C4996 error under MSVC /WX).
        Array<byte> bytes;
        char header[96];
        const int headerLength =
            std::snprintf(header, sizeof(header), "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %u +X %u\n",
                          h, w);
        REQUIRE(headerLength > 0);
        for (int i = 0; i < headerLength; ++i)
        {
            bytes.PushBack(static_cast<byte>(header[i]));
        }
        const f32 v = r > g ? (r > b ? r : b) : (g > b ? g : b);
        u8 rgbe[4] = {0, 0, 0, 0};
        if (v >= 1.0e-32f)
        {
            int e = 0;
            const f32 m = std::frexp(v, &e);
            const f32 scale = m * 256.0f / v;
            rgbe[0] = static_cast<u8>(r * scale);
            rgbe[1] = static_cast<u8>(g * scale);
            rgbe[2] = static_cast<u8>(b * scale);
            rgbe[3] = static_cast<u8>(e + 128);
        }
        for (u32 i = 0; i < w * h; ++i)
        {
            for (u8 c : rgbe)
            {
                bytes.PushBack(static_cast<byte>(c));
            }
        }
        REQUIRE(WriteFile(StringView(reinterpret_cast<const utf8char*>(path)),
                          Span<const byte>(bytes.Data(), bytes.Size()))
                    .IsOk());
    }
}

TEST_CASE("texture.pipeline: an HDR source cooks to BC6H on the desktop (BC) target")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    const char* srcPath = "scratch_texpipe_sky.hdr";
    WriteFlatRadianceHdr(srcPath, 72, 72, 2.0f, 0.5f, 0.25f); // > 64px: past the small escape
    (void)RemoveDirectoryRecursive(u8"scratch_texpipe_hdr_db");
    NativeFileSystem outMount(u8"scratch_texpipe_hdr_db", DefaultAllocator());

    auto cook = [&](texcomp::CompressionChoice choice, Guid& id)
    {
        foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount,
                                                   foundation::core::BinarySerializerFactory(),
                                                   u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(
            choice == texcomp::CompressionChoice::None ? u8"skyRaw" : u8"sky",
            TextureResource::StaticType());
        id = inst->Id();
        TextureAsset asset;
        TextureImporter::ImportEquirectangular(
            StringView(reinterpret_cast<const utf8char*>(srcPath)), asset); // usage = HDR
        asset.compression = choice;
        TextureAssetBuilder builder;
        foundation::vfs::NativeFileSystem srcMount(u8".", foundation::core::DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    };
    Guid compressedId, rawId;
    cook(texcomp::CompressionChoice::Default, compressedId);
    cook(texcomp::CompressionChoice::None, rawId);

    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase outDb(foundation::core::DefaultAllocator(), outMount,
                                               foundation::core::BinarySerializerFactory(),
                                               u8".rasset");
    TextureFactory factory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&factory);

    Proxy<Texture> sky = manager.Bind<Texture>(compressedId);
    REQUIRE(sky);
    CHECK(sky->Width() == 72u);
    CHECK(sky->Height() == 72u);
    CHECK(sky->Format() == rhi::TextureFormat::BC6HRGBUfloat); // the HDR policy row
    CHECK(sky->GpuTexture() != nullptr);

    Proxy<Texture> raw = manager.Bind<Texture>(rawId);
    REQUIRE(raw);
    CHECK(raw->Format() == rhi::TextureFormat::RGBA32Float); // Compression = None still escapes

    std::remove(srcPath);
    (void)RemoveDirectoryRecursive(u8"scratch_texpipe_hdr_db");
}

// ===================================================================================
// DDS sources: GPU-ready levels pass through untouched when they fit the
// asset + target; otherwise level 0 decodes and cooks like any image.
// ===================================================================================
namespace
{
    namespace dds = foundation::image::dds;

    // A procedural RGBA8 level (a gradient that differs per level size).
    Array<u8> GradientLevel(u32 w, u32 h)
    {
        Array<u8> px;
        px.Resize(static_cast<usize>(w) * h * 4);
        for (u32 y = 0; y < h; ++y)
        {
            for (u32 x = 0; x < w; ++x)
            {
                u8* p = px.Data() + (static_cast<usize>(y) * w + x) * 4;
                p[0] = static_cast<u8>((x * 255u) / (w > 1 ? w - 1 : 1));
                p[1] = static_cast<u8>((y * 255u) / (h > 1 ? h - 1 : 1));
                p[2] = static_cast<u8>(((x + y) * 7u) & 0xFFu);
                p[3] = 255;
            }
        }
        return px;
    }

    // A DDS whose every level is the gradient encoded to `blockFormat` by the engine's encoder.
    dds::DdsImage EncodedDds(u32 size, u32 levels, rhi::TextureFormat blockFormat,
                             dds::DdsFormat ddsFormat)
    {
        dds::DdsImage out;
        out.width = size;
        out.height = size;
        out.mipLevels = levels;
        out.format = ddsFormat;
        out.colorSpaceKnown = true;
        u32 w = size;
        for (u32 level = 0; level < levels; ++level)
        {
            const Array<u8> px = GradientLevel(w, w);
            const Array<byte> blocks = texcomp::EncodeBlockCompressed(px.Data(), w, w, blockFormat, 128);
            REQUIRE(blocks.Size() == rhi::CompressedLevelBytes(blockFormat, w, w));
            for (byte b : blocks)
            {
                out.data.PushBack(static_cast<u8>(b));
            }
            w = w > 1 ? w / 2 : 1;
        }
        REQUIRE(out.data.Size() == out.LayerSize());
        return out;
    }

    void WriteDdsFile(const dds::DdsImage& image, StringView path)
    {
        Array<u8> file;
        REQUIRE(dds::WriteDds(image, file).IsOk());
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(file.Data()), file.Size())).IsOk());
    }

    // What a cook produced: the record's facts (the record itself is not copyable) and the "data" payload.
    struct Cooked
    {
        rhi::TextureFormat format = rhi::TextureFormat::RGBA8Unorm;
        u32 width = 0;
        u32 mipLevels = 0;
    };

    // Cook a file-backed asset against `target` (null for the desktop host).
    Status CookFile(const TextureAsset& asset, const pipeline::CookTarget* target, StringView dbDir,
                    Cooked& outResource, Array<u8>& outPayload)
    {
        (void)RemoveDirectoryRecursive(dbDir);
        NativeFileSystem outMount(dbDir, DefaultAllocator());
        foundation::content::ContentDatabase outDb(DefaultAllocator(), outMount,
                                                   foundation::core::BinarySerializerFactory(), u8".rasset");
        auto* inst = outDb.RootGroup()->CreateInstance(u8"out", TextureResource::StaticType());
        REQUIRE(inst != nullptr);
        TextureAssetBuilder builder;
        foundation::vfs::NativeFileSystem srcMount(u8".", DefaultAllocator());
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &srcMount;
        ctx.output = inst;
        ctx.target = target;
        const Status built = builder.Build(asset, ctx);
        if (!built.IsOk())
        {
            (void)RemoveDirectoryRecursive(dbDir);
            return built;
        }
        RefPtr<ISerializable> object = inst->ReadObject();
        auto* res = Cast<TextureResource>(object.Get());
        REQUIRE(res != nullptr);
        outResource.format = res->format;
        outResource.width = res->width;
        outResource.mipLevels = res->mipLevels;
        UniquePtr<IStream> data = inst->ReadData(u8"data");
        REQUIRE(data);
        outPayload.Resize(static_cast<usize>(data->Size()));
        REQUIRE(data->Read(outPayload.Data(), static_cast<u64>(outPayload.Size())) ==
                static_cast<u64>(outPayload.Size()));
        (void)RemoveDirectoryRecursive(dbDir);
        return built;
    }
}

TEST_CASE("texture.pipeline: a BC7 DDS with its mip chain passes through byte-for-byte on a BC target")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    const dds::DdsImage src = EncodedDds(8, 4, rhi::TextureFormat::BC7RGBAUnorm, dds::DdsFormat::BC7Srgb);
    WriteDdsFile(src, u8"scratch_texpipe_bc7.dds");

    TextureAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"scratch_texpipe_bc7.dds");
    asset.SetupFor3D(); // Colour, sRGB, mips
    Cooked res;
    Array<u8> payload;
    REQUIRE(CookFile(asset, nullptr, u8"scratch_texpipe_dds_db", res, payload).IsOk());
    CHECK(res.format == rhi::TextureFormat::BC7RGBAUnormSrgb);
    CHECK(res.width == 8u);
    CHECK(res.mipLevels == 4u);
    REQUIRE(payload.Size() == src.data.Size());
    CHECK(std::memcmp(payload.Data(), src.data.Data(), payload.Size()) == 0);

    // The asset's colour space picks the format twin: the same bytes, the linear view.
    asset.colorSpace = image::ImageColorSpace::Linear;
    REQUIRE(CookFile(asset, nullptr, u8"scratch_texpipe_dds_db", res, payload).IsOk());
    CHECK(res.format == rhi::TextureFormat::BC7RGBAUnorm);
    CHECK(payload.Size() == src.data.Size());

    // No mips asked (the UI preset): level 0 alone passes through.
    asset.SetupForUI();
    REQUIRE(CookFile(asset, nullptr, u8"scratch_texpipe_dds_db", res, payload).IsOk());
    CHECK(res.mipLevels == 1u);
    CHECK(payload.Size() == src.LevelSize(0));
    CHECK(res.format == rhi::TextureFormat::BC7RGBAUnormSrgb);

    // Compression None is the escape hatch: the blocks decode and cook raw (with generated mips).
    asset.SetupFor3D();
    asset.compression = texcomp::CompressionChoice::None;
    REQUIRE(CookFile(asset, nullptr, u8"scratch_texpipe_dds_db", res, payload).IsOk());
    CHECK(res.format == rhi::TextureFormat::RGBA8UnormSrgb);
    CHECK(res.mipLevels == 4u);
    CHECK(payload.Size() == (64u + 16u + 4u + 1u) * 4u);
    FileDelete(u8"scratch_texpipe_bc7.dds");
}

TEST_CASE("texture.pipeline: a BC5 normal-map DDS decodes and re-encodes by the policy (BC7-linear, generated mips)")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    // 128 px: above the policy's small-texture cutoff, so the re-encode is visible.
    dds::DdsImage src;
    src.width = 128;
    src.height = 128;
    src.mipLevels = 1; // a package normal without a chain
    src.format = dds::DdsFormat::BC5;
    src.colorSpaceKnown = true;
    {
        Array<u8> flat;
        flat.Resize(128u * 128u * 4u);
        for (usize i = 0; i < 128u * 128u; ++i)
        {
            flat[i * 4 + 0] = 128;
            flat[i * 4 + 1] = 128;
            flat[i * 4 + 2] = 255;
            flat[i * 4 + 3] = 255;
        }
        const Array<byte> blocks =
            texcomp::EncodeBlockCompressed(flat.Data(), 128, 128, rhi::TextureFormat::BC5RGUnorm, 128);
        REQUIRE(blocks.Size() == rhi::CompressedLevelBytes(rhi::TextureFormat::BC5RGUnorm, 128, 128));
        for (byte b : blocks)
        {
            src.data.PushBack(static_cast<u8>(b));
        }
    }
    WriteDdsFile(src, u8"scratch_texpipe_bc5.dds");

    TextureAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"scratch_texpipe_bc5.dds");
    asset.SetupForNormalMap();
    Cooked res;
    Array<u8> payload;
    REQUIRE(CookFile(asset, nullptr, u8"scratch_texpipe_dds_db", res, payload).IsOk());
    CHECK(res.format == rhi::TextureFormat::BC7RGBAUnorm); // never BC5 (the shaders read rgb)
    CHECK(res.mipLevels == 8u);                             // 128 down to 1
    usize expected = 0;
    for (u32 level = 0, w = 128; level < 8; ++level, w = w > 1 ? w / 2 : 1)
    {
        expected += rhi::CompressedLevelBytes(rhi::TextureFormat::BC7RGBAUnorm, w, w);
    }
    CHECK(payload.Size() == expected);
    FileDelete(u8"scratch_texpipe_bc5.dds");
}

TEST_CASE("texture.pipeline: on an ASTC-only target a BC DDS decodes and cooks ASTC; a cubemap DDS is refused")
{
    RegisterTextureResource();
    RegisterTextureAsset();
    const dds::DdsImage src = EncodedDds(128, 1, rhi::TextureFormat::BC1RGBAUnorm, dds::DdsFormat::BC1Srgb);
    WriteDdsFile(src, u8"scratch_texpipe_bc1.dds");
    TextureAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"scratch_texpipe_bc1.dds");
    asset.SetupFor3D();
    const pipeline::CookTarget mobile{String(u8"web-astc"), false, true, false};
    Cooked res;
    Array<u8> payload;
    REQUIRE(CookFile(asset, &mobile, u8"scratch_texpipe_dds_db", res, payload).IsOk());
    CHECK(res.format == rhi::TextureFormat::ASTC4x4UnormSrgb);
    CHECK(res.mipLevels == 8u);
    FileDelete(u8"scratch_texpipe_bc1.dds");

    dds::DdsImage cube = EncodedDds(4, 1, rhi::TextureFormat::BC1RGBAUnorm, dds::DdsFormat::BC1);
    cube.cubemap = true;
    cube.arrayLayers = 6;
    for (u32 face = 1; face < 6; ++face)
    {
        for (u32 i = 0; i < 8; ++i)
        {
            const u8 b = cube.data[i]; // a copy: the push may reallocate the array
            cube.data.PushBack(b);
        }
    }
    WriteDdsFile(cube, u8"scratch_texpipe_cube.dds");
    asset.fileName = foundation::vfs::SourcePath(u8"scratch_texpipe_cube.dds");
    CHECK(CookFile(asset, nullptr, u8"scratch_texpipe_dds_db", res, payload).Code() == ErrorCode::NotSupported);
    FileDelete(u8"scratch_texpipe_cube.dds");
}

TEST_CASE("texture-import: a DDS imports with the facts its header names")
{
    RegisterTextureAsset();
    const StringView dir = u8"scratch_tex_import_dds_project";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir, u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir);
    REQUIRE(static_cast<bool>(project));
    TextureFileImporter importer;
    CHECK(importer.Accepts(u8"dds"));

    auto importOne = [&](StringView file) -> RefPtr<ISerializable>
    {
        Result<foundation::content::Instance*> imported = importer.Import(
            file, pipeline::ImportContext{DefaultAllocator(), project->SourcesRoot()},
            *project->SourceDb().RootGroup(), nullptr, nullptr, nullptr);
        REQUIRE(imported.HasValue());
        return imported.Value()->ReadObject();
    };

    // BC5 is a normal map, whatever the name says.
    WriteDdsFile(EncodedDds(4, 1, rhi::TextureFormat::BC5RGUnorm, dds::DdsFormat::BC5), u8"thing.dds");
    {
        RefPtr<ISerializable> object = importOne(u8"thing.dds");
        auto* asset = Cast<TextureAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->usage == texcomp::TextureUsage::Normal);
        CHECK(asset->colorSpace == image::ImageColorSpace::Linear);
        CHECK(asset->fileName == StringView(u8"thing.dds"));
        CHECK(FileExists(PathJoin(dir, u8"Sources/thing.dds").AsView()));
    }
    // BC4 is a data mask.
    WriteDdsFile(EncodedDds(4, 1, rhi::TextureFormat::BC4RUnorm, dds::DdsFormat::BC4), u8"stuff.dds");
    {
        RefPtr<ISerializable> object = importOne(u8"stuff.dds");
        auto* asset = Cast<TextureAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->usage == texcomp::TextureUsage::Mask);
    }
    // A DX10 colour format settles the colour space: BC7_UNORM is linear, BC1_UNORM_SRGB is sRGB.
    WriteDdsFile(EncodedDds(4, 1, rhi::TextureFormat::BC7RGBAUnorm, dds::DdsFormat::BC7), u8"wall.dds");
    WriteDdsFile(EncodedDds(4, 1, rhi::TextureFormat::BC1RGBAUnorm, dds::DdsFormat::BC1Srgb), u8"brick.dds");
    {
        RefPtr<ISerializable> object = importOne(u8"wall.dds");
        auto* asset = Cast<TextureAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->usage == texcomp::TextureUsage::Color);
        CHECK(asset->colorSpace == image::ImageColorSpace::Linear);
        CHECK(asset->generateMipmaps); // the 3D preset
    }
    {
        RefPtr<ISerializable> object = importOne(u8"brick.dds");
        auto* asset = Cast<TextureAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->colorSpace == image::ImageColorSpace::Srgb);
    }
    for (StringView f : {u8"thing.dds", u8"stuff.dds", u8"wall.dds", u8"brick.dds"})
    {
        FileDelete(f);
    }
    project.Reset();
    (void)RemoveDirectoryRecursive(dir);
}


TEST_CASE("texture.pipeline: a render texture asset cooks to a record its factory makes a target of")
{
    RegisterTextureAsset();
    FileDelete(u8"scratch_rt_pipe_db/minimap.rasset");
    FileDelete(u8"scratch_rt_pipe_db/huge.rasset");
    RemoveDirectory(u8"scratch_rt_pipe_db");
    NativeFileSystem mount(u8"scratch_rt_pipe_db", DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");

    // The format is an enum by name (the asset form's dropdown).
    CHECK(TypeOf<RenderTextureFormat>().enumeratorCount == 2u);

    RenderTextureAssetBuilder builder;
    CHECK(builder.AssetType() == &RenderTextureAsset::StaticType());
    CHECK(builder.ProductType() == &RenderTextureResource::StaticType());

    auto* minimap =
        db.RootGroup()->CreateInstance(u8"minimap", RenderTextureResource::StaticType());
    RenderTextureAsset asset;
    asset.width = 512;
    asset.height = 128;
    asset.format = RenderTextureFormat::Hdr;
    {
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = minimap;
        ctx.db = &db;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // A side past the limit is refused at the cook, not at the first frame.
    auto* huge = db.RootGroup()->CreateInstance(u8"huge", RenderTextureResource::StaticType());
    RenderTextureAsset tooBig;
    tooBig.width = kRenderTextureResourceMaxSize * 2;
    {
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = huge;
        ctx.db = &db;
        CHECK_FALSE(builder.Build(tooBig, ctx).IsOk());
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    TextureFactory factory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);
    Proxy<Texture> tex = manager.Bind<Texture>(minimap->Id());
    REQUIRE(tex);
    CHECK(tex->Width() == 512u);
    CHECK(tex->Height() == 128u);
    CHECK(tex->Format() == rhi::TextureFormat::RGBA16Float);

    // The asset itself round-trips (what the editor saves and the asset form edits).
    MemoryStream stream;
    {
        BinarySerializer writer(stream, SerializeMode::Write);
        asset.Serialize(writer);
        REQUIRE(writer.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    RenderTextureAsset read;
    {
        BinarySerializer reader(stream, SerializeMode::Read);
        read.Serialize(reader);
        REQUIRE(reader.IsOk());
    }
    CHECK(read.width == 512u);
    CHECK(read.height == 128u);
    CHECK(read.format == RenderTextureFormat::Hdr);
}
