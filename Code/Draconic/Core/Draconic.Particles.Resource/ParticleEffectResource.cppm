// draconic.particles.resource - the cooked ParticleEffectResource (runtime input), its serializer,
// and its resource factory. A ParticleEffectResource IS a reflected ISerializable that holds a
// runtime ParticleEffect; the cook (draconic.particles.editor) writes one into the content DB, the
// factory reconstructs it at Bind. Polymorphic modules round-trip via the reflection/serializable
// registry (Serializables().Create by type-id) - the same machinery TextureResource uses.
//
// See docs/design/particles-authoring.md.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <utility> // std::move

export module draconic.particles.resource;

import draconic.foundation;
import draconic.particles;
import draconic.content;
import draconic.resource;
import draconic.texture;
import draconic.texture.resource;

using namespace draconic::foundation;
namespace content = draconic::content;
namespace resource = draconic::resource;
namespace texture = draconic::texture;

export namespace draconic::particles
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
            foundation::Serialize(ar, "type", typeId);
            RefPtr<ParticleInitializer> created;
            if (reading)
            {
                RefPtr<ISerializable> obj = GlobalSerializableRegistry().Create(typeId);
                if (obj)
                {
                    created = RefPtr<ParticleInitializer>{Cast<ParticleInitializer>(obj.Get())};
                    mod = created.Get();
                }
            }
            if (mod != nullptr)
            {
                mod->Serialize(ar);
            }
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
            foundation::Serialize(ar, "type", typeId);
            RefPtr<ParticleBehavior> created;
            if (reading)
            {
                RefPtr<ISerializable> obj = GlobalSerializableRegistry().Create(typeId);
                if (obj)
                {
                    created = RefPtr<ParticleBehavior>{Cast<ParticleBehavior>(obj.Get())};
                    mod = created.Get();
                }
            }
            if (mod != nullptr)
            {
                mod->Serialize(ar);
            }
            ar.EndObject();
            if (reading && created)
            {
                sys.AddBehavior(std::move(created));
            }
        }
        ar.EndArray();
    }

    inline void SerializeSystem(ISerializer& ar, ParticleEffect& fx, i32 index)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        i32 maxParticles = reading ? 0 : fx.GetSystem(index)->MaxParticles();
        u64 seed = reading ? 0ull : fx.GetSystem(index)->Seed();
        foundation::Serialize(ar, "maxParticles", maxParticles);
        foundation::Serialize(ar, "seed", seed);
        ParticleSystem* sys = reading ? &fx.AddSystem(maxParticles, seed) : fx.GetSystem(index);

        foundation::Serialize(ar, "name", sys->name);
        foundation::Serialize(ar, "desiredMode", sys->desiredMode);
        foundation::Serialize(ar, "simSpace", sys->simulationSpace);
        foundation::Serialize(ar, "blend", sys->blendMode);
        foundation::Serialize(ar, "render", sys->renderMode);
        foundation::Serialize(ar, "textureRef",
                        sys->textureRef); // cooked texture GUID (null = untextured)
        foundation::Serialize(ar, "sort", sys->sortParticles);
        foundation::Serialize(ar, "soft", sys->softParticles);
        foundation::Serialize(ar, "softDistance", sys->softDistance);
        foundation::Serialize(ar, "trail", sys->trail);
        foundation::Serialize(ar, "flipbook", sys->flipbook);
        foundation::Serialize(ar, "prewarm", sys->prewarmTime);
        foundation::Serialize(ar, "lodStart", sys->lodStartDistance);
        foundation::Serialize(ar, "lodCull", sys->lodCullDistance);
        foundation::Serialize(ar, "lodMinRate", sys->lodMinRate);

        ar.Key("emitter");
        ar.BeginObject();
        foundation::Serialize(ar, "mode", sys->emitter.mode);
        foundation::Serialize(ar, "spawnRate", sys->emitter.spawnRate);
        foundation::Serialize(ar, "burstCount", sys->emitter.burstCount);
        foundation::Serialize(ar, "burstInterval", sys->emitter.burstInterval);
        foundation::Serialize(ar, "burstCycles", sys->emitter.burstCycles);
        foundation::Serialize(ar, "isEmitting", sys->emitter.isEmitting);
        foundation::Serialize(ar, "duration", sys->emitter.duration);
        foundation::Serialize(ar, "looping", sys->emitter.looping);
        ar.EndObject();

        SerializeInitializers(ar, *sys);
        SerializeBehaviors(ar, *sys);
    }

    inline void SerializeEffect(ISerializer& ar, ParticleEffect& fx)
    {
        const bool reading = ar.Mode() == SerializeMode::Read;
        foundation::Serialize(ar, "name", fx.name);

        u32 systemCount = reading ? 0u : static_cast<u32>(fx.SystemCount());
        ar.Key("systems");
        ar.BeginArray(systemCount);
        for (u32 i = 0; i < systemCount; ++i)
        {
            ar.BeginObject();
            SerializeSystem(ar, fx, static_cast<i32>(i));
            ar.EndObject();
        }
        ar.EndArray();

        const Span<const SubEmitterLink> links = fx.SubEmitterLinks();
        u32 linkCount = reading ? 0u : static_cast<u32>(links.Size());
        ar.Key("links");
        ar.BeginArray(linkCount);
        for (u32 i = 0; i < linkCount; ++i)
        {
            ar.BeginObject();
            SubEmitterLink link = reading ? SubEmitterLink{} : links[static_cast<usize>(i)];
            Serialize(ar, link);
            ar.EndObject();
            if (reading)
            {
                fx.AddSubEmitterLink(link);
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

export namespace draconic::particles
{
    // ---- Cooked resource ---------------------------------------------------------------------
    // Both the cooked record AND the runtime product (no GPU transform needed): holds a template
    // ParticleEffect. A component instantiates its own ParticleEffectInstance over this effect.
    class ParticleEffectResource final : public ISerializable
    {
        DRACONIC_OBJECT(ParticleEffectResource, ISerializable)
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

    private:
        ParticleEffect m_effect;
        Array<resource::Proxy<texture::Texture>> m_systemTextures;
    };

    // ---- Factory -----------------------------------------------------------------------------
    // Data factory (model B): deserialize the record; no GPU upload. Referenced cooked resources
    // (textures/meshes/materials), when added, are attached here via manager.Bind<T> (dependency edges).
    class ParticleEffectFactory final : public resource::IResourceFactory
    {
    public:
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ParticleEffectResource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(resource::ResourceManager& manager,
                                            content::Instance& instance) override
        {
            RefPtr<ISerializable> obj = instance.ReadObject();
            ParticleEffectResource* res = Cast<ParticleEffectResource>(obj.Get());
            if (res != nullptr)
            {
                // Resolve each system's texture GUID to a Proxy<Texture>. Bind records a dependency edge,
                // so re-cooking the texture transitively reloads this effect; the Proxy then follows it.
                Array<resource::Proxy<texture::Texture>> textures(DefaultAllocator());
                ParticleEffect& fx = res->Effect();
                for (i32 s = 0; s < fx.SystemCount(); ++s)
                {
                    ParticleSystem* sys = fx.GetSystem(s);
                    const bool hasTex = (sys != nullptr) && !(sys->textureRef == Guid{});
                    textures.PushBack(hasTex ? manager.Bind<texture::Texture>(sys->textureRef)
                                             : resource::Proxy<texture::Texture>{});
                }
                res->SetSystemTextures(Move(textures));
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

    DRACONIC_DEFINE_OBJECT(ParticleEffectResource, "draconic::particles")
}
