// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles.resource - the cooked ParticleEffectResource (runtime input), its serializer,
// and its resource factory. A ParticleEffectResource IS a reflected ISerializable that holds a
// runtime ParticleEffect; the cook (foundation.particles.editor) writes one into the content DB, the
// factory reconstructs it at Bind. Polymorphic modules round-trip via the reflection/serializable
// registry (Serializables().Create by type-id) - the same machinery TextureResource uses.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <utility> // std::move

export module foundation.particles.resource;

import foundation.core;
import foundation.particles;
import foundation.content;
import foundation.resource;
import foundation.texture;
import foundation.texture.resource;
import foundation.geometry;          // StaticMesh (mesh-mode particles)
import foundation.geometry.resource; // the StaticMesh resource factory (for manager.Bind)
import foundation.materials;          // Material (mesh-mode particles)
import foundation.materials.resource; // the Material resource factory (for manager.Bind)

using namespace foundation::core;
namespace core = foundation::core;
namespace content = foundation::content;
namespace resource = foundation::resource;
namespace texture = foundation::texture;
namespace geometry = foundation::geometry;
namespace materials = foundation::materials;

export namespace foundation::particles
{
    // ---- Effect serializer (bidirectional; ported from Sedulous ParticleEffectSerializer) --------

    // A polymorphic module: write its reflected type-id (u64) + params; on read, reconstruct via the
    // serializable registry, read its params, and add it to the system (which declares its streams).
    inline void SerializeInitializers(ISerializer& ar, ParticleSystem& sys)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        u32 count = reading ? 0u : static_cast<u32>(sys.InitializerCount());
        ar.Key("initializers");
        ar.BeginArray(count);
        for (u32 i = 0; i < count; ++i)
        {
            ar.BeginObject();
            ParticleInitializer* mod = reading ? nullptr : sys.GetInitializer(static_cast<i32>(i));
            u64 typeId = reading ? 0ull : mod->GetType()->id;
            core::Serialize(ar, "type", typeId);
            RefPtr<ParticleInitializer> created;
            if (reading)
            {
                RefPtr<ISerializable> obj = GlobalSerializableRegistry().Create(typeId);
                created = obj ? RefPtr<ParticleInitializer>{Cast<ParticleInitializer>(obj.Get())} : RefPtr<ParticleInitializer>{};
                if (!created)
                {
                    // A module this build cannot construct is NOT skippable: its parameters stay
                    // in the positional stream and everything after shifts (a garbage particle
                    // budget, a runaway allocation). Refuse the payload, like a stale data
                    // version; the factory binds nothing.
                    ar.FailPayload(ErrorCode::NotSupported);
                    return;
                }
                mod = created.Get();
            }
            mod->Serialize(ar);
            ar.EndObject();
            if (reading && created)
            {
                sys.AddInitializer(std::move(created));
            }
        }
        ar.EndArray();
    }

    inline void SerializeBehaviors(ISerializer& ar, ParticleSystem& sys)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        u32 count = reading ? 0u : static_cast<u32>(sys.BehaviorCount());
        ar.Key("behaviors");
        ar.BeginArray(count);
        for (u32 i = 0; i < count; ++i)
        {
            ar.BeginObject();
            ParticleBehavior* mod = reading ? nullptr : sys.GetBehavior(static_cast<i32>(i));
            u64 typeId = reading ? 0ull : mod->GetType()->id;
            core::Serialize(ar, "type", typeId);
            RefPtr<ParticleBehavior> created;
            if (reading)
            {
                RefPtr<ISerializable> obj = GlobalSerializableRegistry().Create(typeId);
                created = obj ? RefPtr<ParticleBehavior>{Cast<ParticleBehavior>(obj.Get())} : RefPtr<ParticleBehavior>{};
                if (!created)
                {
                    // A module this build cannot construct is NOT skippable: its parameters stay
                    // in the positional stream and everything after shifts (a garbage particle
                    // budget, a runaway allocation). Refuse the payload, like a stale data
                    // version; the factory binds nothing.
                    ar.FailPayload(ErrorCode::NotSupported);
                    return;
                }
                mod = created.Get();
            }
            mod->Serialize(ar);
            ar.EndObject();
            if (reading && created)
            {
                sys.AddBehavior(std::move(created));
            }
        }
        ar.EndArray();
    }

    // `effectLinks`: the layout before a system owned its sub-emitters (see SerializeEffect), with none.
    inline void SerializeSystem(ISerializer& ar, ParticleEffect& fx, i32 index, bool effectLinks = false)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        i32 maxParticles = reading ? 0 : fx.GetSystem(index)->MaxParticles();
        u64 seed = reading ? 0ull : fx.GetSystem(index)->Seed();
        core::Serialize(ar, "maxParticles", maxParticles);
        core::Serialize(ar, "seed", seed);
        ParticleSystem* sys = reading ? &fx.AddSystem(maxParticles, seed) : fx.GetSystem(index);

        core::Serialize(ar, "name", sys->name);
        core::Serialize(ar, "desiredMode", sys->desiredMode);
        core::Serialize(ar, "simSpace", sys->simulationSpace);
        core::Serialize(ar, "blend", sys->blendMode);
        core::Serialize(ar, "render", sys->renderMode);
        core::Serialize(ar, "textureRef",
                        sys->textureRef); // cooked texture GUID (null = untextured)
        core::Serialize(ar, "meshRef", sys->meshRef); // cooked mesh GUID (null = no effect mesh)
        core::Serialize(ar, "meshScale", sys->meshScale);
        core::Serialize(ar, "materialRefs",
                        sys->materialRefs); // per-submesh cooked material GUIDs (empty = none)
        core::Serialize(ar, "sort", sys->sortParticles);
        core::Serialize(ar, "soft", sys->softParticles);
        core::Serialize(ar, "softDistance", sys->softDistance);
        core::Serialize(ar, "trail", sys->trail);
        core::Serialize(ar, "flipbook", sys->flipbook);
        core::Serialize(ar, "prewarm", sys->prewarmTime);
        core::Serialize(ar, "lodStart", sys->lodStartDistance);
        core::Serialize(ar, "lodCull", sys->lodCullDistance);
        core::Serialize(ar, "lodMinRate", sys->lodMinRate);

        ar.Key("emitter");
        ar.BeginObject();
        core::Serialize(ar, "mode", sys->emitter.mode);
        core::Serialize(ar, "spawnRate", sys->emitter.spawnRate);
        core::Serialize(ar, "burstCount", sys->emitter.burstCount);
        core::Serialize(ar, "burstInterval", sys->emitter.burstInterval);
        core::Serialize(ar, "burstCycles", sys->emitter.burstCycles);
        core::Serialize(ar, "isEmitting", sys->emitter.isEmitting);
        core::Serialize(ar, "duration", sys->emitter.duration);
        core::Serialize(ar, "looping", sys->emitter.looping);
        ar.EndObject();

        SerializeInitializers(ar, *sys);
        if (!ar.IsPayloadOk())
        {
            return;
        }
        SerializeBehaviors(ar, *sys);
        if (!ar.IsPayloadOk() || effectLinks)
        {
            return;
        }
        // Its sub-emitters, each into the system it names.
        u32 linkCount = reading ? 0u : static_cast<u32>(sys->subEmitters.Size());
        ar.Key("subEmitters");
        ar.BeginArray(linkCount);
        if (reading)
        {
            sys->subEmitters.Clear();
        }
        for (u32 i = 0; i < linkCount; ++i)
        {
            ar.BeginObject();
            SubEmitterLink link = reading ? SubEmitterLink{} : sys->subEmitters[static_cast<usize>(i)];
            Serialize(ar, link);
            ar.EndObject();
            if (reading)
            {
                sys->subEmitters.PushBack(link);
            }
        }
        ar.EndArray();
    }

    /// An effect either way. `effectLinks`: the layout before a system owned its sub-emitters, the
    /// effect's own list of links each into a system on every other system's births or deaths
    /// (the authoring asset's data version 0); read, each becomes a sub-emitter of every system but
    /// the one it spawns into, as it behaved.
    inline void SerializeEffect(ISerializer& ar, ParticleEffect& fx, bool effectLinks = false)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        if (reading)
        {
            fx.Clear(); // read over an effect (an editor's undo), not added to what it had
        }
        core::Serialize(ar, "name", fx.name);

        u32 systemCount = reading ? 0u : static_cast<u32>(fx.SystemCount());
        ar.Key("systems");
        ar.BeginArray(systemCount);
        for (u32 i = 0; i < systemCount; ++i)
        {
            ar.BeginObject();
            SerializeSystem(ar, fx, static_cast<i32>(i), reading && effectLinks);
            if (!ar.IsPayloadOk())
            {
                return; // refused above: do not read the next system's budget from a shifted stream
            }
            ar.EndObject();
        }
        ar.EndArray();

        if (!reading || !effectLinks)
        {
            return;
        }
        u32 linkCount = 0;
        ar.Key("links");
        ar.BeginArray(linkCount);
        for (u32 i = 0; i < linkCount; ++i)
        {
            ar.BeginObject();
            SubEmitterLink link{};
            Serialize(ar, link);
            ar.EndObject();
            for (i32 s = 0; s < fx.SystemCount(); ++s)
            {
                if (link.childSystemIndex >= 0 && link.childSystemIndex < fx.SystemCount() && s != link.childSystemIndex)
                {
                    fx.GetSystem(s)->subEmitters.PushBack(link); // into nothing, it did nothing: dropped
                }
            }
        }
        ar.EndArray();
    }

    // Deep-copy an effect via a serialize round-trip (reuses the one serializer; truly independent -
    // no shared module RefPtrs). This is the core of the bake: the editor clones the authored asset's
    // effect into a fresh cooked resource (later: resolving refs / baking LUTs during the copy).
    inline void CloneEffect(const ParticleEffect& src, ParticleEffect& dst)
    {
        MemoryStream buffer(DefaultAllocator());
        {
            BinarySerializer writer(buffer, SerializeMode::Write);
            SerializeEffect(writer,
                            const_cast<ParticleEffect&>(
                                src)); // write pass only reads src (bidirectional API is non-const)
        }
        (void)buffer.Seek(0, SeekOrigin::Begin);
        BinarySerializer reader(buffer, SerializeMode::Read);
        SerializeEffect(reader, dst);
    }
}

export namespace foundation::particles
{
    // ---- Cooked resource ---------------------------------------------------------------------
    // Both the cooked record AND the runtime product (no GPU transform needed): holds a template
    // ParticleEffect. A component instantiates its own ParticleEffectInstance over this effect.
    class ParticleEffectResource final : public ISerializable
    {
        RTTI_OBJECT(ParticleEffectResource, ISerializable)
    public:
        [[nodiscard]] ParticleEffect& Effect() noexcept { return m_effect; }
        [[nodiscard]] const ParticleEffect& Effect() const noexcept { return m_effect; }
        void Serialize(ISerializer& ar) override { SerializeEffect(ar, m_effect); }

        // Per-system resolved texture handles (parallel to Effect().GetSystem(i)), bound by the factory
        // from each system's textureRef GUID. A Proxy follows its resource handle, so a hot-reloaded
        // texture is picked up without rebinding. Null Proxy = untextured system.
        [[nodiscard]] resource::Proxy<texture::Texture> SystemTexture(i32 systemIndex) const
        {
            return (systemIndex >= 0 && systemIndex < static_cast<i32>(m_systemTextures.Size()))
                       ? m_systemTextures[static_cast<usize>(systemIndex)]
                       : resource::Proxy<texture::Texture>{};
        }
        void SetSystemTextures(Array<resource::Proxy<texture::Texture>> textures)
        {
            m_systemTextures = Move(textures);
        }

        // Per-system resolved mesh handles (parallel to the systems), bound by the factory from each
        // system's meshRef GUID - the mesh drawn per particle in Mesh render mode. Null = no effect mesh.
        [[nodiscard]] resource::Proxy<geometry::StaticMesh> SystemMesh(i32 systemIndex) const
        {
            return (systemIndex >= 0 && systemIndex < static_cast<i32>(m_systemMeshes.Size()))
                       ? m_systemMeshes[static_cast<usize>(systemIndex)]
                       : resource::Proxy<geometry::StaticMesh>{};
        }
        void SetSystemMeshes(Array<resource::Proxy<geometry::StaticMesh>> meshes)
        {
            m_systemMeshes = Move(meshes);
        }

        // Per-system resolved material lists (parallel to the systems; the inner list is per-submesh,
        // indexed by SubMesh::materialIndex), bound by the factory from each system's materialRefs. Slot 0
        // is the whole-mesh material. Empty = no effect material. Mesh render mode only.
        [[nodiscard]] const Array<resource::Proxy<materials::Material>>&
        SystemMaterials(i32 systemIndex) const
        {
            static const Array<resource::Proxy<materials::Material>> kEmpty;
            return (systemIndex >= 0 && systemIndex < static_cast<i32>(m_systemMaterials.Size()))
                       ? m_systemMaterials[static_cast<usize>(systemIndex)]
                       : kEmpty;
        }
        void SetSystemMaterials(Array<Array<resource::Proxy<materials::Material>>> mats)
        {
            m_systemMaterials = Move(mats);
        }

    private:
        ParticleEffect m_effect;
        Array<resource::Proxy<texture::Texture>> m_systemTextures;
        Array<resource::Proxy<geometry::StaticMesh>> m_systemMeshes;
        Array<Array<resource::Proxy<materials::Material>>> m_systemMaterials;
    };

    // Resolve a resource's per-system texture/mesh/material GUID refs (read off its effect's systems)
    // into Proxy<T> handles via `manager` (Bind records the dependency edges, so re-cooking a referenced
    // asset transitively reloads this effect and the Proxy follows it). Shared by the cooked FACTORY and
    // by the editor's LIVE PREVIEW, which resolves an in-memory resource built from the authored effect -
    // so a preview renders every mesh system's mesh + materials exactly like a cooked dist.
    inline void ResolveParticleEffectResources(ParticleEffectResource& res,
                                               resource::ResourceManager& manager)
    {
        ParticleEffect& fx = res.Effect();

        Array<resource::Proxy<texture::Texture>> textures(DefaultAllocator());
        for (i32 s = 0; s < fx.SystemCount(); ++s)
        {
            ParticleSystem* sys = fx.GetSystem(s);
            const bool hasTex = (sys != nullptr) && !(sys->textureRef == Guid{});
            textures.PushBack(hasTex ? manager.Bind<texture::Texture>(sys->textureRef)
                                     : resource::Proxy<texture::Texture>{});
        }
        res.SetSystemTextures(Move(textures));

        Array<resource::Proxy<geometry::StaticMesh>> meshes(DefaultAllocator());
        for (i32 s = 0; s < fx.SystemCount(); ++s)
        {
            ParticleSystem* sys = fx.GetSystem(s);
            const bool hasMesh = (sys != nullptr) && !(sys->meshRef == Guid{});
            meshes.PushBack(hasMesh ? manager.Bind<geometry::StaticMesh>(sys->meshRef)
                                    : resource::Proxy<geometry::StaticMesh>{});
        }
        res.SetSystemMeshes(Move(meshes));

        // Per-submesh material lists (slot 0 = whole-mesh); a null slot GUID stays an unbound Proxy.
        Array<Array<resource::Proxy<materials::Material>>> mats(DefaultAllocator());
        for (i32 s = 0; s < fx.SystemCount(); ++s)
        {
            ParticleSystem* sys = fx.GetSystem(s);
            Array<resource::Proxy<materials::Material>> perSubmesh(DefaultAllocator());
            if (sys != nullptr)
            {
                for (usize m = 0; m < sys->materialRefs.Size(); ++m)
                {
                    const Guid& ref = sys->materialRefs[m];
                    perSubmesh.PushBack(!(ref == Guid{}) ? manager.Bind<materials::Material>(ref)
                                                         : resource::Proxy<materials::Material>{});
                }
            }
            mats.PushBack(Move(perSubmesh));
        }
        res.SetSystemMaterials(Move(mats));
    }

    // ---- Factory -----------------------------------------------------------------------------
    // Data factory (model B): deserialize the record; no GPU upload. Referenced cooked resources
    // (textures/meshes/materials) are resolved to Proxies via ResolveParticleEffectResources.
    class ParticleEffectFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ParticleEffectResource::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &ParticleEffectResource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            content::Instance& instance) override
        {
            RefPtr<ISerializable> obj = instance.ReadObject();
            if (ParticleEffectResource* res = Cast<ParticleEffectResource>(obj.Get()))
            {
                ResolveParticleEffectResources(*res, manager);
            }
            return obj;
        }
    };

    // Register the cooked resource type + all module types. Call once at startup (tooling and runtime).
    inline void RegisterParticleEffectResource()
    {
        RegisterParticleModules();
        GlobalTypeRegistry().Register(ParticleEffectResource::StaticType());
        RegisterSerializable<ParticleEffectResource>();
    }

    // v1 (2026-10-21): curves of Core's keys and gradients, size as `size`/`separateAxes`/`sizeY`
    // (cooked again: the builder's version moved). v2 (2026-10-27): each system its sub-emitters,
    // no effect-level links (cooked again).
    RTTI_DEFINE_OBJECT_VERSIONED(ParticleEffectResource, "rtti::particles", 2)
}

export namespace foundation::particles
{
    /// The particles resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    inline constexpr foundation::resource::ResourceFactoryDesc kParticlesResourceFactories[] = {
        foundation::resource::FactoryByDefault<ParticleEffectResource, ParticleEffectResource, ParticleEffectFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kParticlesResourceModule{
        u8"particles", &RegisterParticleEffectResource, kParticlesResourceFactories,
        sizeof(kParticlesResourceFactories) / sizeof(kParticlesResourceFactories[0])};
}
