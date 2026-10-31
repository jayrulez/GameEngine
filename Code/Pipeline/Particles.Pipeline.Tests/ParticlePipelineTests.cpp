// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The full authoring bake: author a ParticleEffectAsset in code, cook it with
// ParticleEffectAssetBuilder into a content-DB instance, then load the cooked ParticleEffectResource
// back through the ResourceManager and factory and confirm it round-trips and simulates. Shaped
// like TexturePipelineTests, adapted for an authored (not imported) asset.
#include <doctest/doctest.h>
#include <string>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.xml.serialization; // XmlSerializerFactory (a version 0 asset read as the editor reads it)
import foundation.resource;
import pipeline.core;
import foundation.rhi;
import foundation.rhi.null;
import foundation.texture;
import foundation.texture.resource;
import foundation.particles;
import foundation.particles.resource;
import particles.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::particles;
namespace rhi = foundation::rhi;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_pfx_edit_db/smoke.rasset");
        RemoveDirectory(u8"scratch_pfx_edit_db");
    }
}

TEST_CASE("particles.pipeline: authored asset -> Build() -> cooked resource -> Bind -> run")
{
    RegisterParticleEffectAsset();
    RemoveTree();

    NativeFileSystem mount(u8"scratch_pfx_edit_db", DefaultAllocator());
    Guid id;

    // Author the effect in an asset, then cook it via the builder into the DB instance.
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst =
            db.RootGroup()->CreateInstance(u8"smoke", ParticleEffectResource::StaticType());
        id = inst->Id();

        ParticleEffectAsset asset;
        ParticleSystem& sys = asset.Effect().AddSystem(1000, 777ull);
        sys.name = String(u8"smoke");
        sys.blendMode = ParticleBlendMode::Alpha;
        sys.emitter.spawnRate = 40.0f;
        sys.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(2.0f, 3.0f);
        sys.AddInitializer<SizeInitializer>().size = RangeFloat2::Constant(Float2{1.0f, 1.0f});
        sys.AddBehavior<DragBehavior>().drag = 0.3f;
        sys.AddBehavior<SizeOverLifetimeBehavior>().size =
            ParticleCurveFloat::Ease(1, 3); // an eased size, both axes

        ParticleEffectAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = inst;
        ctx.db = &db;
        REQUIRE(builder.AssetType() == &ParticleEffectAsset::StaticType());
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Load the cooked resource back.
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    ParticleEffectFactory factory;
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<ParticleEffectResource> res = manager.Bind<ParticleEffectResource>(id);
    REQUIRE(res);
    ParticleEffect& fx = res->Effect();
    REQUIRE(fx.SystemCount() == 1);

    ParticleSystem* sys = fx.GetSystem(0);
    REQUIRE(sys != nullptr);
    CHECK(sys->MaxParticles() == 1000);
    CHECK(sys->Seed() == 777ull);
    CHECK(sys->blendMode == ParticleBlendMode::Alpha);
    CHECK(sys->emitter.spawnRate == doctest::Approx(40.0f));
    CHECK(sys->InitializerCount() == 2);
    CHECK(sys->BehaviorCount() == 2);

    ParticleEffectInstance inst(fx);
    inst.Update(0.1f);
    CHECK(sys->AliveCount() > 0);

    RemoveTree();
}

TEST_CASE("particles.pipeline: Build resolves a texture path ref -> cooked GUID -> bound Proxy")
{
    RegisterParticleEffectAsset();
    foundation::texture::RegisterTextureResource();
    FileDelete(u8"scratch_pfx_ref_db/smoketex.rasset");
    FileDelete(u8"scratch_pfx_ref_db/smoketex.data.bin");
    FileDelete(u8"scratch_pfx_ref_db/effect.rasset");
    RemoveDirectory(u8"scratch_pfx_ref_db");

    NativeFileSystem mount(u8"scratch_pfx_ref_db", DefaultAllocator());
    Guid effectId, texId;

    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");

        // Cook a texture the effect will reference.
        auto* texInst = db.RootGroup()->CreateInstance(
            u8"smoketex", foundation::texture::TextureResource::StaticType());
        texId = texInst->Id();
        foundation::texture::TextureResource tr;
        tr.width = 2;
        tr.height = 2;
        tr.format = rhi::TextureFormat::RGBA8Unorm;
        REQUIRE(texInst->WriteObject(tr).IsOk());
        u8 px[2 * 2 * 4] = {};
        REQUIRE(texInst
                    ->WriteData(u8"data",
                                Span<const byte>(reinterpret_cast<const byte*>(px), sizeof(px)))
                    .IsOk());

        // Author an effect that references the texture by path, then cook it.
        auto* fxInst =
            db.RootGroup()->CreateInstance(u8"effect", ParticleEffectResource::StaticType());
        effectId = fxInst->Id();
        ParticleEffectAsset asset;
        ParticleSystem& sys = asset.Effect().AddSystem(500);
        sys.renderMode = ParticleRenderMode::Billboard;
        sys.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(1.0f, 1.0f);
        asset.SetSystemTexturePath(0, u8"smoketex");

        ParticleEffectAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.output = fxInst;
        ctx.db = &db;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Load with both factories so the effect's Create can Bind the referenced texture.
    rhi::null::NullDevice device{DefaultAllocator()};
    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    ParticleEffectFactory pfxFactory;
    foundation::texture::TextureFactory texFactory(DefaultAllocator(), device);
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&pfxFactory);
    manager.AddFactory(&texFactory);

    Proxy<ParticleEffectResource> res = manager.Bind<ParticleEffectResource>(effectId);
    REQUIRE(res);

    // The path was resolved to the texture's cooked GUID during Build,
    CHECK(res->Effect().GetSystem(0)->textureRef == texId);
    // and the factory bound it to a live, hot-reload-following Proxy<Texture>.
    Proxy<foundation::texture::Texture> tex = res->SystemTexture(0);
    REQUIRE(tex);
    CHECK(tex->GpuTexture() != nullptr);

    FileDelete(u8"scratch_pfx_ref_db/smoketex.rasset");
    FileDelete(u8"scratch_pfx_ref_db/smoketex.data.bin");
    FileDelete(u8"scratch_pfx_ref_db/effect.rasset");
    RemoveDirectory(u8"scratch_pfx_ref_db");
}

TEST_CASE("particles.pipeline: a version 0 asset's effect links are read onto its systems")
{
    // FireworksV0.xasset is an effect as it was saved before a system owned its sub-emitters:
    // Shells and Sparks, and the effect's one link, into Sparks. Read through its envelope as an
    // editor reads it, the link is the shells' own; the sparks have none.
    RegisterParticleEffectAsset();
    const String fixture = PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Code/Pipeline/Particles.Pipeline.Tests/FireworksV0.xasset");
    Result<Array<byte>> bytes = ReadFile(fixture.AsView());
    REQUIRE(bytes.HasValue());
    String text(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));

    (void)RemoveDirectoryRecursive(u8"scratch_pfx_v0_db");
    (void)CreateDirectory(u8"scratch_pfx_v0_db");
    {
        NativeFileSystem mount(u8"scratch_pfx_v0_db", DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, foundation::xml::XmlSerializerFactory(), u8".xasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"Fireworks", ParticleEffectAsset::StaticType());
        REQUIRE(inst != nullptr);
        // The fixture's own id is another's: the instance's in its place.
        utf8char id[37];
        inst->Id().ToChars(id);
        const StringView old(u8"38dc5399-3e9d-4863-b7af-b61c991a4875");
        const StringView all = text.AsView();
        usize at = 0;
        while (at + old.Size() <= all.Size() && all.SubStr(at, old.Size()) != old)
            ++at;
        REQUIRE(at + old.Size() <= all.Size());
        String envelope(all.SubStr(0, at));
        envelope.Append(StringView(id, 36));
        envelope.Append(all.SubStr(at + old.Size(), all.Size() - at - old.Size()));
        MemoryStream stream;
        (void)stream.Write(envelope.CStr(), envelope.Size());
        (void)stream.Seek(0, SeekOrigin::Begin);
        Result<RefPtr<ISerializable>, String> read = inst->ReadObjectFrom(stream);
        if (!read.HasValue())
            MESSAGE(std::string(reinterpret_cast<const char*>(read.Error().CStr())));
        REQUIRE(read.HasValue());
        auto* asset = Cast<ParticleEffectAsset>(read.Value().Get());
        REQUIRE(asset != nullptr);
        ParticleEffect& fx = asset->Effect();
        REQUIRE(fx.SystemCount() == 2);
        REQUIRE(fx.GetSystem(0)->subEmitters.Size() == 1u);
        CHECK(fx.GetSystem(0)->subEmitters[0].childSystemIndex == 1);
        CHECK(fx.GetSystem(0)->subEmitters[0].spawnCount == 40);
        CHECK(fx.GetSystem(1)->subEmitters.IsEmpty());
    }
    (void)RemoveDirectoryRecursive(u8"scratch_pfx_v0_db");
}

TEST_CASE("particles.pipeline: an effect declares what its systems draw with as references, so a cook of it cooks them")
{
    ParticleEffectAsset asset;
    ParticleEffect& fx = asset.Effect();
    (void)fx.AddSystem(64);
    (void)fx.AddSystem(64);
    ParticleSystem* sparks = fx.GetSystem(0);
    ParticleSystem* debris = fx.GetSystem(1);
    REQUIRE(sparks != nullptr);
    REQUIRE(debris != nullptr);
    const Guid texture{0x11u, 0x1u};
    const Guid mesh{0x22u, 0x2u};
    const Guid material{0x33u, 0x3u};
    sparks->textureRef = texture;
    debris->meshRef = mesh;
    debris->materialRefs.PushBack(Guid{});
    debris->materialRefs.PushBack(material);
    ParticleEffectAssetBuilder builder;
    AssetBuildContext ctx{DefaultAllocator()};
    AssetDependencies deps;
    builder.ScanDependencies(asset, ctx, deps);
    REQUIRE(deps.references.Size() == 3u); // the empty slot left out
    CHECK(deps.references[0] == texture);
    CHECK(deps.references[1] == mesh);
    CHECK(deps.references[2] == material);
    CHECK(deps.reads.IsEmpty()); // runtime references: an edit of them re-cooks nothing
}
