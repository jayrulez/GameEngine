// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles.editor - the edit-time ParticleEffectAsset and its builder (the bake).
// Tooling only; the runtime and apps never link this. Unlike an imported asset (a texture importing
// an external .png, say), a particle effect is authored, so the asset embeds the effect itself and
// Build() cooks it into a ParticleEffectResource with no source-file load. Build resolves each
// system's texture path to its cooked guid; the rest of the effect passes through as authored.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module particles.pipeline;

import foundation.core;
import pipeline.core;
import foundation.particles;
import foundation.particles.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation;
using namespace foundation::particles;
namespace core = foundation::core;
namespace content = foundation::content;

export namespace pipeline{
    // Edit-time asset: the authored effect (embedded, since unlike an imported texture there is no
    // external source file). Serialized to a readable .particlefx (XmlSerializer) as fileName and the
    // effect graph. This is a distinct type from the cooked ParticleEffectResource; Build() transforms
    // one into the other.
    class ParticleEffectAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ParticleEffectAsset, pipeline::Asset)
    public:
        [[nodiscard]] ParticleEffect& Effect() noexcept { return m_effect; }
        [[nodiscard]] const ParticleEffect& Effect() const noexcept { return m_effect; }

        // Edit-time texture reference per system, as an asset path (soft ref). Build() resolves each to
        // the referenced cooked texture's GUID. Empty means untextured.
        void SetSystemTexturePath(i32 systemIndex, StringView path)
        {
            if (systemIndex < 0)
            {
                return;
            }
            while (static_cast<i32>(m_systemTexturePaths.Size()) <= systemIndex)
            {
                m_systemTexturePaths.PushBack(String{});
            }
            m_systemTexturePaths[static_cast<usize>(systemIndex)] = String(path);
        }
        [[nodiscard]] StringView SystemTexturePath(i32 systemIndex) const
        {
            return (systemIndex >= 0 && systemIndex < static_cast<i32>(m_systemTexturePaths.Size()))
                       ? m_systemTexturePaths[static_cast<usize>(systemIndex)].AsView()
                       : StringView{};
        }

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (unused for authored effects)
            // the authored effect graph; a version 0 asset's effect-level links read onto its systems
            SerializeEffect(ar, m_effect, ar.Mode() == SerializeMode::Read && ar.Version() < 1);
            core::Serialize(ar, "texturePaths", m_systemTexturePaths); // edit-time soft refs
        }

    private:
        ParticleEffect m_effect;
        Array<String> m_systemTexturePaths; // per-system texture asset paths (edit-time)
    };

    // The bake: cook a ParticleEffectAsset into the output content Instance as a ParticleEffectResource.
    class ParticleEffectAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ParticleEffectAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &ParticleEffectResource::StaticType();
        }
        // v2: the cooked resource carries per-system meshRef and meshScale (Mesh render mode).
        // v3: per-system materialRef (the effect-level material for Mesh render mode).
        // v4: materialRef became the materialRefs list (per submesh; slot 0 is the whole-mesh material).
        // v5: curves of Core's keys and gradients; size over lifetime is `size`, `separateAxes` and
        // `sizeY` (the cooked resource's data version 1).
        // v6: each system its sub-emitters (the cooked resource's data version 2).
        // Each bump makes stale products re-cook in the new format (the strict reader needs the new keys).
        [[nodiscard]] u32 Version() const override { return 6; }
        // What each system draws with is a runtime reference (the factory binds it at load): its
        // texture, its mesh and its materials, by id, or a texture by its edit-time path. Declared
        // so a cook of the effect cooks them too (otherwise a scene's first cook would reach the
        // effect but not its texture), and so asset_uses and the export's reach see them.
        void ScanDependencies(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx, pipeline::AssetDependencies& out) override
        {
            const ParticleEffectAsset& pa = static_cast<const ParticleEffectAsset&>(asset);
            const ParticleEffect& fx = pa.Effect();
            const auto add = [&out](const Guid& id)
            {
                if (!id.IsNil())
                    out.references.PushBack(id);
            };
            for (i32 s = 0; s < fx.SystemCount(); ++s)
            {
                const ParticleSystem* system = const_cast<ParticleEffect&>(fx).GetSystem(s);
                if (system == nullptr)
                    continue;
                add(system->textureRef);
                add(system->meshRef);
                for (const Guid& material : system->materialRefs)
                    add(material);
                const StringView path = pa.SystemTexturePath(s);
                if (!path.IsEmpty() && ctx.db != nullptr)
                    if (content::Instance* named = ctx.db->GetInstance(path))
                        add(named->Id());
            }
        }
        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            const ParticleEffectAsset& pa = static_cast<const ParticleEffectAsset&>(asset);
            // Bake: build a fresh cooked resource from the authored effect (a faithful deep copy),
            ParticleEffectResource cooked;
            CloneEffect(pa.Effect(), cooked.Effect());
            // then resolve refs: each system's edit-time texture path becomes the referenced cooked
            // resource's GUID (written into the cooked system's textureRef; the factory binds it at load).
            ParticleEffect& fx = cooked.Effect();
            for (i32 s = 0; s < fx.SystemCount(); ++s)
            {
                const StringView path = pa.SystemTexturePath(s);
                if (path.IsEmpty())
                {
                    continue;
                }
                content::Instance* dep = (ctx.db != nullptr) ? ctx.db->GetInstance(path) : nullptr;
                if (dep == nullptr)
                {
                    return Status{ErrorCode::NotFound};
                } // referenced asset must be cooked first
                fx.GetSystem(s)->textureRef = dep->Id();
            }
            return ctx.output->WriteObject(cooked);
        }
    };

    // Register the asset type, the cooked resource, and the modules it depends on. Tooling side.
    inline void RegisterParticleEffectAsset()
    {
        RegisterParticleEffectResource();
        GlobalTypeRegistry().Register(ParticleEffectAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ParticleEffectAsset>();
    }

    // Data version 0 holds curves of Core's keys and gradients, with size as one curve. Data
    // version 1: each system owns its sub-emitters; version 0's effect-level links are read onto
    // the systems (the legacy reader, SerializeEffect's effectLinks).

    /// A new effect's content: one continuous fountain system.
    void SeedDefaultParticleEffect(foundation::particles::ParticleEffect& effect);
    /// Adds the default system (a continuous fountain: a lifetime, a velocity, a size, a colour and
    /// gravity) after the effect's others; what the seed and the editor's Add System make.
    foundation::particles::ParticleSystem& AddDefaultParticleSystem(foundation::particles::ParticleEffect& effect);
    /// File > New's particle creator (pipeline.registration composes every domain's).
    void RegisterParticleCreators(AssetCreatorRegistry& registry);
}
