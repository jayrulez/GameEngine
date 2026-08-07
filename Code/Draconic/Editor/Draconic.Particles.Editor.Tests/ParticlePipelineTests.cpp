// The full authoring bake: author a ParticleEffectAsset in code, cook it with
// ParticleEffectAssetBuilder into a content-DB instance, then load the cooked ParticleEffectResource
// back through the ResourceManager + factory and confirm it round-trips + simulates. Mirrors
// TexturePipelineTests, adapted for an AUTHORED (not imported) asset.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.vfs;
import draconic.content;
import draconic.resource;
import draconic.editor;
import draconic.rhi;
import draconic.rhi.null;
import draconic.texture;
import draconic.texture.resource;
import draconic.particles;
import draconic.particles.resource;
import draconic.particles.editor;

using namespace draconic::foundation;
using namespace draconic::vfs;
using namespace draconic::resource;
using namespace draconic::particles;
namespace rhi = draconic::rhi;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"draconic_pfx_edit_db/smoke.rasset");
        RemoveDirectory(u8"draconic_pfx_edit_db");
    }
}

TEST_CASE("particles.pipeline: authored asset -> Build() -> cooked resource -> Bind -> run")
{
    RegisterParticleEffectAsset();
    RemoveTree();

    NativeFileSystem mount(u8"draconic_pfx_edit_db");
    Guid id;

    // Author the effect in an asset, then cook it via the builder into the DB instance.
    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
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
        sys.AddBehavior<SizeOverLifetimeBehavior>().curve =
            ParticleCurveFloat2::Linear(Float2{1, 1}, Float2{3, 3});

        ParticleEffectAssetBuilder builder;
        draconic::editor::AssetBuildContext ctx;
        ctx.output = inst;
        ctx.db = &db;
        REQUIRE(builder.AssetType() == &ParticleEffectAsset::StaticType());
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Load the cooked resource back.
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    ParticleEffectFactory factory;
    ResourceManager manager(db);
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
    draconic::texture::RegisterTextureResource();
    FileDelete(u8"draconic_pfx_ref_db/smoketex.rasset");
    FileDelete(u8"draconic_pfx_ref_db/smoketex.data.bin");
    FileDelete(u8"draconic_pfx_ref_db/effect.rasset");
    RemoveDirectory(u8"draconic_pfx_ref_db");

    NativeFileSystem mount(u8"draconic_pfx_ref_db");
    Guid effectId, texId;

    {
        draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                              u8".rasset");

        // Cook a texture the effect will reference.
        auto* texInst = db.RootGroup()->CreateInstance(
            u8"smoketex", draconic::texture::TextureResource::StaticType());
        texId = texInst->Id();
        draconic::texture::TextureResource tr;
        tr.width = 2;
        tr.height = 2;
        tr.format = rhi::TextureFormat::RGBA8Unorm;
        REQUIRE(texInst->WriteObject(tr).IsOk());
        u8 px[2 * 2 * 4] = {};
        REQUIRE(texInst
                    ->WriteData(u8"data",
                                Span<const byte>(reinterpret_cast<const byte*>(px), sizeof(px)))
                    .IsOk());

        // Author an effect that references the texture BY PATH, then cook it.
        auto* fxInst =
            db.RootGroup()->CreateInstance(u8"effect", ParticleEffectResource::StaticType());
        effectId = fxInst->Id();
        ParticleEffectAsset asset;
        ParticleSystem& sys = asset.Effect().AddSystem(500);
        sys.renderMode = ParticleRenderMode::Billboard;
        sys.AddInitializer<LifetimeInitializer>().lifetime = RangeFloat(1.0f, 1.0f);
        asset.SetSystemTexturePath(0, u8"smoketex");

        ParticleEffectAssetBuilder builder;
        draconic::editor::AssetBuildContext ctx;
        ctx.output = fxInst;
        ctx.db = &db;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    // Load with both factories so the effect's Create can Bind the referenced texture.
    rhi::null::NullDevice device{DefaultAllocator()};
    draconic::content::ContentDatabase db(mount, draconic::foundation::BinarySerializerFactory(),
                                          u8".rasset");
    ParticleEffectFactory pfxFactory;
    draconic::texture::TextureFactory texFactory(device);
    ResourceManager manager(db);
    manager.AddFactory(&pfxFactory);
    manager.AddFactory(&texFactory);

    Proxy<ParticleEffectResource> res = manager.Bind<ParticleEffectResource>(effectId);
    REQUIRE(res);

    // The path was resolved to the texture's cooked GUID during Build...
    CHECK(res->Effect().GetSystem(0)->textureRef == texId);
    // ...and the factory bound it to a live, hot-reload-following Proxy<Texture>.
    Proxy<draconic::texture::Texture> tex = res->SystemTexture(0);
    REQUIRE(tex);
    CHECK(tex->GpuTexture() != nullptr);

    FileDelete(u8"draconic_pfx_ref_db/smoketex.rasset");
    FileDelete(u8"draconic_pfx_ref_db/smoketex.data.bin");
    FileDelete(u8"draconic_pfx_ref_db/effect.rasset");
    RemoveDirectory(u8"draconic_pfx_ref_db");
}
