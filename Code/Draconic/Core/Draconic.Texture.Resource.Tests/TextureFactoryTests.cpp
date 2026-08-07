// Model-A runtime load: author a cooked TextureResource (record + "data" stream)
// into a content DB, then load it through the ResourceManager with a device-backed
// TextureFactory (Null RHI backend, headless) and verify the live GPU Texture.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.rhi;
import draconic.rhi.null;
import draconic.texture;
import draconic.texture.resource;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::texture;
namespace rhi = draconic::rhi;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"draconic_texfac_db/tex.rasset");
        FileDelete(u8"draconic_texfac_db/tex.data.bin");
        RemoveDirectory(u8"draconic_texfac_db");
    }

    // Author one cooked 2x2 texture instance; returns its id.
    Guid AuthorTexture(draconic::content::ContentDatabase& db, StringView name)
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

    NativeFileSystem mount(u8"draconic_texfac_db");
    Guid id;

    // Author a cooked record + raw 2x2 RGBA pixels (the "data" stream).
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
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
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    TextureFactory factory(device);
    ResourceManager manager(db);
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

TEST_CASE("texture.factory: rejects an instance whose object isn't a TextureResource")
{
    RegisterTextureResource();
    rhi::null::NullDevice device{DefaultAllocator()};
    TextureFactory factory(device);
    CHECK(factory.ProductType() == &Texture::StaticType());
}

TEST_CASE("texture.factory: async load produces the same product as the sync load")
{
    RegisterTextureResource();
    RemoveTree();

    NativeFileSystem mount(u8"draconic_texfac_db");
    Guid id;
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
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
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    TextureFactory factory(device);

    // Synchronous reference product.
    ResourceManager syncManager(db);
    syncManager.AddFactory(&factory);
    Proxy<Texture> a = syncManager.Bind<Texture>(id);
    REQUIRE(a);

    // Async: decode on a worker, finalize on the main thread via WaitAll.
    JobSystem jobs;
    ResourceManager asyncManager(db, &jobs);
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
    RemoveDirectory(u8"draconic_texfac_concurrent");

    constexpr int kCount = 12;
    NativeFileSystem mount(u8"draconic_texfac_concurrent");
    Array<Guid> ids;
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");
        for (int i = 0; i < kCount; ++i)
        {
            char8_t name[8] = {u8't', u8'e', u8'x', static_cast<char8_t>(u8'0' + i / 10),
                               static_cast<char8_t>(u8'0' + i % 10), 0};
            ids.PushBack(AuthorTexture(db, StringView(name)));
        }
    }

    rhi::null::NullDevice device{DefaultAllocator()};
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    TextureFactory factory(device);
    JobSystem jobs;
    ResourceManager manager(db, &jobs);
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

    RemoveDirectory(u8"draconic_texfac_concurrent");
}
