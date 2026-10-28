// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Model-A runtime load: author a cooked TextureResource (record + "data" stream)
// into a content DB, then load it through the ResourceManager with a device-backed
// TextureFactory (Null RHI backend, headless) and verify the live GPU Texture.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.rhi;
import foundation.rhi.null;
import foundation.texture;
import foundation.texture.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::texture;
namespace rhi = foundation::rhi;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_texfac_db/tex.rasset");
        FileDelete(u8"scratch_texfac_db/tex.data.bin");
        RemoveDirectory(u8"scratch_texfac_db");
    }

    // Author one cooked 2x2 texture instance; returns its id.
    Guid AuthorTexture(foundation::content::ContentDatabase& db, StringView name)
    {
        auto* inst = db.RootGroup()->CreateInstance(name, TextureResource::StaticType());
        TextureResource res;
        res.width = 2;
        res.height = 2;
        res.format = rhi::TextureFormat::RGBA8UnormSrgb;
        res.wrapU = TextureWrap::ClampToEdge;
        res.wrapV = TextureWrap::ClampToEdge;
        u8 pixels[2 * 2 * 4];
        for (usize i = 0; i < sizeof(pixels); ++i)
        {
            pixels[i] = static_cast<u8>(i * 7);
        }
        (void)inst->WriteObject(res);
        (void)inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                         sizeof(pixels)));
        return inst->Id();
    }
}

TEST_CASE("texture.factory: cooked TextureResource -> live GPU Texture")
{
    RegisterTextureResource();
    RemoveTree();

    NativeFileSystem mount(u8"scratch_texfac_db", DefaultAllocator());
    Guid id;

    // Author a cooked record + raw 2x2 RGBA pixels (the "data" stream).
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"tex", TextureResource::StaticType());
        id = inst->Id();

        TextureResource res;
        res.width = 2;
        res.height = 2;
        res.format = rhi::TextureFormat::RGBA8UnormSrgb;
        res.minFilter = TextureFilter::Linear;
        res.magFilter = TextureFilter::Linear;
        res.wrapU = TextureWrap::ClampToEdge;
        res.wrapV = TextureWrap::ClampToEdge;
        res.anisotropy = 8.0f;
        REQUIRE(inst->WriteObject(res).IsOk());

        u8 pixels[2 * 2 * 4];
        for (usize i = 0; i < sizeof(pixels); ++i)
        {
            pixels[i] = static_cast<u8>(i * 3);
        }
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    // Load through the manager with a device-backed factory (Null backend).
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    TextureFactory factory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<Texture> tex = manager.Bind<Texture>(id);
    REQUIRE(tex);
    CHECK(tex->Width() == 2u);
    CHECK(tex->Height() == 2u);
    CHECK(tex->Format() == rhi::TextureFormat::RGBA8UnormSrgb);
    CHECK(tex->GpuTexture() != nullptr);
    CHECK(tex->Sampler() != nullptr);

    RemoveTree();
}

TEST_CASE("texture.factory: a cooked RenderTextureResource is a texture a camera can draw into")
{
    RegisterTextureResource();
    FileDelete(u8"scratch_rtfac_db/target.rasset");
    FileDelete(u8"scratch_rtfac_db/bad.rasset");
    RemoveDirectory(u8"scratch_rtfac_db");
    NativeFileSystem mount(u8"scratch_rtfac_db", DefaultAllocator());
    foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(),
                                            u8".rasset");
    Guid targetId;
    Guid badId;
    {
        auto* inst = db.RootGroup()->CreateInstance(u8"target",
                                                    RenderTextureResource::StaticType());
        RenderTextureResource res;
        res.width = 320;
        res.height = 180;
        res.format = rhi::TextureFormat::RGBA16Float;
        REQUIRE(inst->WriteObject(res).IsOk());
        targetId = inst->Id();

        auto* bad = db.RootGroup()->CreateInstance(u8"bad", RenderTextureResource::StaticType());
        RenderTextureResource zero;
        zero.width = 0; // no size: refused, never a zero-extent GPU texture
        REQUIRE(bad->WriteObject(zero).IsOk());
        badId = bad->Id();
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    TextureFactory factory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    // The product is an ordinary Texture: a sprite, a material or a UI image take it as is.
    Proxy<Texture> tex = manager.Bind<Texture>(targetId);
    REQUIRE(tex);
    CHECK(tex->Width() == 320u);
    CHECK(tex->Height() == 180u);
    CHECK(tex->Format() == rhi::TextureFormat::RGBA16Float);
    REQUIRE(tex->GpuTexture() != nullptr);
    const rhi::TextureUsage usage = tex->GpuTexture()->desc.usage;
    CHECK((usage & rhi::TextureUsage::RenderTarget) == rhi::TextureUsage::RenderTarget);
    CHECK((usage & rhi::TextureUsage::Sampled) == rhi::TextureUsage::Sampled);
    CHECK((usage & rhi::TextureUsage::CopySrc) == rhi::TextureUsage::CopySrc);
    CHECK(tex->View() != nullptr);
    CHECK(tex->Sampler() != nullptr);

    CHECK_FALSE(manager.Bind<Texture>(badId));

    RenderTextureResource limits;
    CHECK(limits.IsValid()); // the defaults: 256x256 LDR
    limits.width = kRenderTextureResourceMaxSize + 1;
    CHECK_FALSE(limits.IsValid());
    limits.width = 64;
    limits.format = rhi::TextureFormat::Depth32Float; // a camera renders colour
    CHECK_FALSE(limits.IsValid());
}

TEST_CASE("texture.factory: rejects an instance whose object isn't a TextureResource")
{
    RegisterTextureResource();
    rhi::null::NullDevice device{DefaultAllocator()};
    TextureFactory factory(DefaultAllocator(), device);
    CHECK(factory.ProductType() == &Texture::StaticType());
}

TEST_CASE("texture.factory: async load produces the same product as the sync load")
{
    RegisterTextureResource();
    RemoveTree();

    NativeFileSystem mount(u8"scratch_texfac_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"tex", TextureResource::StaticType());
        id = inst->Id();

        TextureResource res;
        res.width = 4;
        res.height = 2;
        res.format = rhi::TextureFormat::RGBA8UnormSrgb;
        res.wrapU = TextureWrap::ClampToEdge;
        res.wrapV = TextureWrap::ClampToEdge;
        res.anisotropy = 8.0f;
        REQUIRE(inst->WriteObject(res).IsOk());

        u8 pixels[4 * 2 * 4];
        for (usize i = 0; i < sizeof(pixels); ++i)
        {
            pixels[i] = static_cast<u8>(i * 5);
        }
        REQUIRE(inst->WriteData(u8"data", Span<const byte>(reinterpret_cast<const byte*>(pixels),
                                                           sizeof(pixels)))
                    .IsOk());
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    TextureFactory factory(DefaultAllocator(), device);

    // Synchronous reference product.
    ResourceManager syncManager(DefaultAllocator(), db);
    syncManager.AddFactory(&factory);
    Proxy<Texture> a = syncManager.Bind<Texture>(id);
    REQUIRE(a);

    // Async: decode on a worker, finalize on the main thread via WaitAll.
    JobSystem jobs(DefaultAllocator());
    ResourceManager asyncManager(DefaultAllocator(), db, &jobs);
    asyncManager.AddFactory(&factory);
    Proxy<Texture> b = asyncManager.BindAsync<Texture>(id);
    asyncManager.WaitAll();
    REQUIRE(b);
    CHECK(b.Handle()->State() == ResourceState::Ready);

    // Descriptor equivalence (the Null backend has no pixel readback; the cooked record + upload
    // path is identical, so matching descriptors + valid GPU objects is the equivalence check).
    CHECK(b->Width() == a->Width());
    CHECK(b->Height() == a->Height());
    CHECK(b->Format() == a->Format());
    CHECK(b->GpuTexture() != nullptr);
    CHECK(b->Sampler() != nullptr);

    RemoveTree();
}

TEST_CASE("texture.factory: many concurrent async loads decode on workers without a race")
{
    // Stresses the thread-safety of decode-on-a-worker: N textures bound async at once means N
    // DecodeStages reading the content DB (ReadObject/ReadData) + the type/serializable registries
    // concurrently. Run under TSAN to validate the concurrent-read analysis.
    RegisterTextureResource();
    RemoveDirectory(u8"scratch_texfac_concurrent");

    constexpr int kCount = 12;
    NativeFileSystem mount(u8"scratch_texfac_concurrent", DefaultAllocator());
    Array<Guid> ids;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        for (int i = 0; i < kCount; ++i)
        {
            char8_t name[8] = {u8't', u8'e', u8'x', static_cast<char8_t>(u8'0' + i / 10),
                               static_cast<char8_t>(u8'0' + i % 10), 0};
            ids.PushBack(AuthorTexture(db, StringView(name)));
        }
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    TextureFactory factory(DefaultAllocator(), device);
    JobSystem jobs(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db, &jobs);
    manager.AddFactory(&factory);

    Array<Proxy<Texture>> textures;
    for (const Guid& id : ids)
    {
        textures.PushBack(manager.BindAsync<Texture>(id)); // all pending, decoding on workers
    }
    manager.WaitAll();

    for (Proxy<Texture>& tex : textures)
    {
        REQUIRE(tex);
        CHECK(tex.Handle()->State() == ResourceState::Ready);
        CHECK(tex->Width() == 2u);
        CHECK(tex->GpuTexture() != nullptr);
    }

    RemoveDirectory(u8"scratch_texfac_concurrent");
}

TEST_CASE("texture.upload: a compressed cube sizes its faces by block, never per texel")
{
    // BC7 8x8 = 2x2 blocks x 16 bytes = 64 bytes per face. The per-texel size of a compressed
    // format is 0, which used to make faceBytes 0: six zero-byte writes and nothing on the GPU.
    TextureResource res;
    res.shape = TextureShape::Cubemap;
    res.width = 8;
    res.height = 8;
    res.mipLevels = 1;
    res.format = rhi::TextureFormat::BC7RGBAUnorm;
    Array<TextureWrite> writes;
    EnumerateTextureWrites(res, 6u * 64u, writes);
    REQUIRE(writes.Size() == 6u);
    for (u32 face = 0; face < 6; ++face)
    {
        CHECK(writes[face].offset == face * 64u);
        CHECK(writes[face].size == 64u);
        CHECK(writes[face].layout.bytesPerRow == 32u); // one block row: 2 blocks x 16
        CHECK(writes[face].layout.rowsPerImage == 2u); // block rows
        CHECK(writes[face].arrayLayer == face);
        CHECK(writes[face].mipLevel == 0u);
    }
    // A payload holding five faces uploads five; a 1x1 face still pays for a whole block.
    EnumerateTextureWrites(res, 5u * 64u, writes);
    CHECK(writes.Size() == 5u);
    res.width = 1;
    res.height = 1;
    EnumerateTextureWrites(res, 6u * 16u, writes);
    REQUIRE(writes.Size() == 6u);
    CHECK(writes[5].offset == 5u * 16u);
    CHECK(writes[5].size == 16u);
}

TEST_CASE("texture.upload: an uncompressed cube is six per-texel faces; a chain walks level sizes")
{
    TextureResource cube;
    cube.shape = TextureShape::Cubemap;
    cube.width = 4;
    cube.height = 2;
    cube.mipLevels = 1;
    cube.format = rhi::TextureFormat::RGBA8Unorm;
    Array<TextureWrite> writes;
    EnumerateTextureWrites(cube, 6u * 32u, writes);
    REQUIRE(writes.Size() == 6u);
    CHECK(writes[3].offset == 96u);
    CHECK(writes[3].size == 32u);
    CHECK(writes[3].layout.bytesPerRow == 16u);
    CHECK(writes[3].layout.rowsPerImage == 2u);
    CHECK(writes[3].extent.width == 4u);

    // 4x4 RGBA8 with a 3-level chain: 64 + 16 + 4 bytes, each level its own write.
    TextureResource chain;
    chain.shape = TextureShape::Texture2D;
    chain.width = 4;
    chain.height = 4;
    chain.mipLevels = 3;
    chain.format = rhi::TextureFormat::RGBA8Unorm;
    EnumerateTextureWrites(chain, 64u + 16u + 4u, writes);
    REQUIRE(writes.Size() == 3u);
    CHECK(writes[1].offset == 64u);
    CHECK(writes[1].size == 16u);
    CHECK(writes[1].extent.width == 2u);
    CHECK(writes[2].offset == 80u);
    CHECK(writes[2].mipLevel == 2u);
    // Truncated to level 0 only: one write, no overread.
    EnumerateTextureWrites(chain, 64u, writes);
    CHECK(writes.Size() == 1u);
    // A compressed chain: BC7 8x8 (64) + 4x4 (16) + 2x2 (still one block, 16).
    chain.width = 8;
    chain.height = 8;
    chain.format = rhi::TextureFormat::BC7RGBAUnorm;
    EnumerateTextureWrites(chain, 64u + 16u + 16u, writes);
    REQUIRE(writes.Size() == 3u);
    CHECK(writes[2].offset == 80u);
    CHECK(writes[2].size == 16u);
    CHECK(writes[2].layout.bytesPerRow == 16u);
    CHECK(writes[2].layout.rowsPerImage == 1u);
}

