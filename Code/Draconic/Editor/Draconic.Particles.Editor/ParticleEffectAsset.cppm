// draconic.particles.editor - the edit-time ParticleEffectAsset + its builder (the bake). Tooling
// only; the runtime/app never links this. Unlike an imported asset (e.g. a texture importing an
// external .png), a particle effect is AUTHORED - so the asset embeds the effect itself and Build()
// cooks it into a ParticleEffectResource with no source-file load. Ref resolution + curve->LUT baking
// are later transforms; v1 is a straight pass-through of the authored effect.
//
// See docs/design/particles-authoring.md.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.particles.editor;

import draconic.foundation;
import draconic.editor;
import draconic.particles;
import draconic.particles.resource;
import draconic.content;

using namespace draconic::foundation;
namespace content = draconic::content;

export namespace draconic::particles
{
    // Edit-time asset: the authored effect (embedded, since there is no external source file - unlike an
    // imported texture). Serialized to a readable .particlefx (XmlSerializer) as fileName + the effect
    // graph. This is a distinct type from the cooked ParticleEffectResource; Build() transforms one into
    // the other.
    class ParticleEffectAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(ParticleEffectAsset, draconic::editor::Asset)
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
            draconic::editor::Asset::Serialize(ar); // fileName (unused for authored effects)
            SerializeEffect(ar, m_effect);          // the authored effect graph
            foundation::Serialize(ar, "texturePaths", m_systemTexturePaths); // edit-time soft refs
        }

    private:
        ParticleEffect m_effect;
        Array<String> m_systemTexturePaths; // per-system texture asset paths (edit-time)
    };

    // The bake: cook a ParticleEffectAsset into the output content Instance as a ParticleEffectResource.
    class ParticleEffectAssetBuilder final : public draconic::editor::DefaultAssetBuilder
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
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
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
        GlobalTypeRegistry().Register(ParticleEffectAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<ParticleEffectAsset>();
    }

    DRACONIC_DEFINE_OBJECT(ParticleEffectAsset, "draconic::particles")
}
