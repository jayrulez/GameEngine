// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.particles.editor - the edit-time ParticleEffectAsset + its builder (the bake). Tooling
// only; the runtime/app never links this. Unlike an imported asset (e.g. a texture importing an
// external .png), a particle effect is AUTHORED - so the asset embeds the effect itself and Build()
// cooks it into a ParticleEffectResource with no source-file load. Ref resolution + curve->LUT baking
// are later transforms; v1 is a straight pass-through of the authored effect.

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
    // Edit-time asset: the authored effect (embedded, since there is no external source file - unlike an
    // imported texture). Serialized to a readable .particlefx (XmlSerializer) as fileName + the effect
    // graph. This is a distinct type from the cooked ParticleEffectResource; Build() transforms one into
    // the other.
    class ParticleEffectAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(ParticleEffectAsset, pipeline::Asset)
    public:
        [[nodiscard]] ParticleEffect& Effect() noexcept { return m_effect; }
        [[nodiscard]] const ParticleEffect& Effect() const noexcept { return m_effect; }

        // Edit-time texture reference per system, as an asset PATH (soft ref). Build() resolves each to
        // the referenced cooked texture's GUID. Empty = untextured.
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
        // v2 (2026-08-17): the cooked resource carries per-system meshRef + meshScale (Mesh render mode).
        // v3 (2026-08-18): + per-system materialRef (the effect-level material for Mesh render mode).
        // v4 (2026-08-18): materialRef -> materialRefs list (per-submesh, slot 0 = whole-mesh material).
        // v5 (2026-10-21): curves of Core's keys and gradients; size over lifetime is `size`,
        // `separateAxes` and `sizeY` (the cooked resource's data version 1).
        // v6 (2026-10-27): each system its sub-emitters (the cooked resource's data version 2).
        // Bumped so stale products re-cook with the new format (the strict reader needs the new keys).
        [[nodiscard]] u32 Version() const override { return 6; }
        // What each system draws with is a runtime REFERENCE (the factory binds it at load): its
        // texture, its mesh and its materials, by id, or a texture by its edit-time path. Declared
        // so a cook of the effect cooks them too (a scene's first cook reached the effect but not
        // its texture), and asset_uses and the export's reach see them.
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
            // Bake: build a fresh cooked resource from the authored effect (faithful deep copy)...
            ParticleEffectResource cooked;
            CloneEffect(pa.Effect(), cooked.Effect());
            // ...then RESOLVE refs: each system's edit-time texture PATH -> the referenced cooked
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

    // Register the asset type (+ the cooked resource + modules it depends on). Tooling-side.
    inline void RegisterParticleEffectAsset()
    {
        RegisterParticleEffectResource();
        GlobalTypeRegistry().Register(ParticleEffectAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<ParticleEffectAsset>();
    }

    // Data version 0: the curve layout of 2026-10-21 (Core's keys, gradients, size as one curve)
    // moved by re-saving every sample through Tools.Mcp, version 0 having no legacy floor to keep.
    // Data version 1 (2026-10-27): each system owns its sub-emitters; version 0's effect-level
    // links are read onto the systems (the legacy reader, SerializeEffect's effectLinks).

    /// A new effect's content: one continuous fountain system.
    void SeedDefaultParticleEffect(foundation::particles::ParticleEffect& effect);
    /// Adds the default system (a continuous fountain: a lifetime, a velocity, a size, a colour and
    /// gravity) after the effect's others; what the seed and the editor's Add System make.
    foundation::particles::ParticleSystem& AddDefaultParticleSystem(foundation::particles::ParticleEffect& effect);
    /// File > New's particle creator (pipeline.registration composes every domain's).
    void RegisterParticleCreators(AssetCreatorRegistry& registry);
}
