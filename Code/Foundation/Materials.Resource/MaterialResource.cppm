// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Materials.Resource - the `foundation.materials.resource` module.
///
/// Materials as resources: a `MaterialSource` (authored content - references a shader
/// by Guid, plus declared properties + render-state presets + the default-uniform
/// blob) is built by `MaterialFactory` into a runtime `Material`. The factory Binds
/// the referenced `ShaderResource` mid-build, which AUTOMATICALLY records a
/// material→shader dependency edge (see foundation.resource): reloading the shader
/// transitively reloads the material. The product is the data-only `Material`; its
/// GPU bind-group layout is still inferred later by the MaterialSystem.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module foundation.materials.resource;

import foundation.core;
import foundation.rhi;
import foundation.resource;
import foundation.content;
import foundation.shaders;
import foundation.shaders.resource;
import foundation.shaders.system; // ShaderSystem: the effects' tables, a builtin shader's variants
import foundation.texture;
import foundation.texture.resource;
import foundation.materials;

using namespace foundation::core;
using namespace foundation::resource;
namespace rhi = foundation::rhi;
namespace shaders = foundation::shaders;
namespace texture = foundation::texture;

export namespace foundation::materials
{

    // One effect group of a material as stored (Specs/wind-for-every-shader.md): the effect's name
    // and variant flag, its gate's index, its buffer's size, its parameters' names, types, offsets
    // and sizes, and the material's values. Everything the group needs is here, so a material from
    // an asset draws its effect with nothing at runtime but its source.
    struct MaterialEffectGroupSource
    {
        String name;
        u32 flag = 0; // ShaderFlags: the effect's variant
        u32 gateIndex = kNoMaterialEffectGate;
        u32 uniformSize = 0;
        Array<String> propertyNames;
        Array<u8> propertyTypes; // MaterialPropertyType
        Array<u32> propertyOffsets;
        Array<u32> propertySizes;
        Array<u8> values;
    };

    // Found by argument-dependent lookup from foundation.core's Array<T> serialization.
    inline void Serialize(ISerializer& ar, MaterialEffectGroupSource& group)
    {
        ar.BeginObject();
        foundation::core::Serialize(ar, "name", group.name);
        foundation::core::Serialize(ar, "flag", group.flag);
        foundation::core::Serialize(ar, "gateIndex", group.gateIndex);
        foundation::core::Serialize(ar, "uniformSize", group.uniformSize);
        foundation::core::Serialize(ar, "propertyNames", group.propertyNames);
        foundation::core::Serialize(ar, "propertyTypes", group.propertyTypes);
        foundation::core::Serialize(ar, "propertyOffsets", group.propertyOffsets);
        foundation::core::Serialize(ar, "propertySizes", group.propertySizes);
        foundation::core::Serialize(ar, "values", group.values);
        ar.EndObject();
    }

    // Authored material: a shader reference (by resource id) + declared properties (as
    // parallel arrays so it serializes with the primitive Array<T> path) + render-state
    // presets + the packed default-uniform blob. Texture/sampler defaults are resolved at
    // runtime (white/normal fallbacks), so they are not stored.
    class MaterialSource final : public ISerializable
    {
        RTTI_OBJECT(MaterialSource, ISerializable)
    public:
        String name;
        Guid shaderId;     // a cooked ShaderResource; nil -> use shaderName (a builtin)
        String shaderName; // builtin shader name fallback (when shaderId is nil)
        u32 shaderFlags = 0;
        BlendMode blendMode = BlendMode::Opaque;
        DepthMode depthMode = DepthMode::ReadWrite;
        CullModeConfig cullMode = CullModeConfig::Back;
        VertexLayoutType vertexLayout = VertexLayoutType::Mesh;
        // Sampler address modes (rhi::AddressMode values: 0 Repeat, 1 MirrorRepeat,
        // 2 ClampToEdge) - wired from the source asset's sampler at import (v2).
        u8 samplerU = 0;
        u8 samplerV = 0;

        Array<String> propertyNames;
        Array<u8> propertyTypes; // MaterialPropertyType
        Array<u32> propertyBindings;
        Array<u32> propertyOffsets;
        Array<u32> propertySizes;
        Array<u8> uniformDefaults;

        // Default texture bindings: slot name -> texture product guid (parallel arrays). The
        // factory resolves each through the manager (recording the dependency edge, so a texture
        // hot reload cascades into this material) and sets it as the material's default texture.
        // This makes a cooked material SELF-CONTAINED - previously only the model-spawn composite
        // wired textures, so a directly-referenced material rendered untextured.
        Array<String> textureSlots;
        Array<Guid> textureIds;

        // The effect groups (data version 4): one per effect the material's shader declares the
        // variant of, apart from the shader's properties (Specs/wind-for-every-shader.md).
        Array<MaterialEffectGroupSource> effects;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "name", name);
            foundation::core::Serialize(ar, "shaderId", shaderId);
            foundation::core::Serialize(ar, "shaderName", shaderName);
            foundation::core::Serialize(ar, "shaderFlags", shaderFlags);
            foundation::core::Serialize(ar, "blendMode", blendMode);
            foundation::core::Serialize(ar, "depthMode", depthMode);
            foundation::core::Serialize(ar, "cullMode", cullMode);
            foundation::core::Serialize(ar, "vertexLayout", vertexLayout);
            foundation::core::Serialize(ar, "propertyNames", propertyNames);
            foundation::core::Serialize(ar, "propertyTypes", propertyTypes);
            foundation::core::Serialize(ar, "propertyBindings", propertyBindings);
            foundation::core::Serialize(ar, "propertyOffsets", propertyOffsets);
            foundation::core::Serialize(ar, "propertySizes", propertySizes);
            foundation::core::Serialize(ar, "uniformDefaults", uniformDefaults);
            foundation::core::Serialize(ar, "textureSlots", textureSlots);
            foundation::core::Serialize(ar, "textureIds", textureIds);
            foundation::core::Serialize(ar, "samplerU", samplerU);
            foundation::core::Serialize(ar, "samplerV", samplerV);
            // The effect groups came with data version 4. A version-3 payload (a product cooked
            // before them, read through ReadsDataVersionsFrom) has none. A MaterialSource nested in
            // a MaterialAsset (a text .xasset) is read inside the asset's version scope, which does
            // not name MaterialSource (Version() of it is 0 there): an asset saved before the
            // groups has no key, which SerializeAppended reads as none.
            const u32 stored = ar.Mode() == SerializeMode::Read ? ar.Version(MaterialSource::StaticType().id)
                                                                : 0u;
            if (stored == 0u || stored >= 4u)
            {
                SerializeAppended(ar, "effects", effects);
            }
            if (ar.Mode() == SerializeMode::Read)
            {
                AdoptTemplateColorTypes();
                MigrateWindLanes();
            }
        }

        // Before effect groups (data version 3), a PBR material carried the wind in its buffer's
        // spare lanes as the properties WindStrength, WindSpeed and WindHeight (offsets 24, 28,
        // 60). The wind is an effect now: the three properties go (their bytes stay, as the
        // padding the buffer declares there) and the material gets a Wind group holding their
        // values. The group's layout below is the effect's on the day of that change, frozen with
        // the data it reads; the live table is wind.hlsli's effect block, onto which the factory
        // re-lays the group, keeping these values, whenever it has the shader system. Only for a
        // builtin shader's material whose template no longer declares the lanes: a custom shader
        // may name properties of its own so. Idempotent: a migrated source has no lanes left.
        void MigrateWindLanes()
        {
            if (!shaderId.IsNil())
            {
                return;
            }
            static constexpr StringView kLanes[] = {u8"WindStrength", u8"WindSpeed", u8"WindHeight"};
            static constexpr StringView kParameters[] = {u8"Strength", u8"Speed", u8"Height"};
            static constexpr f32 kFallback[] = {0.0f, 1.0f, 1.0f}; // a lane the source lacks
            bool any = false;
            for (const String& n : propertyNames)
            {
                for (StringView lane : kLanes)
                {
                    any = any || n.AsView() == lane;
                }
            }
            if (!any)
            {
                return;
            }
            RefPtr<Material> templ = BuiltinMaterialTemplate(shaderName.AsView());
            if (!templ || templ->FindProperty(kLanes[0]) != nullptr)
            {
                return;
            }

            f32 values[3] = {kFallback[0], kFallback[1], kFallback[2]};
            usize w = 0;
            const usize count = propertyNames.Size();
            for (usize i = 0; i < count; ++i)
            {
                isize lane = -1;
                for (usize k = 0; k < 3; ++k)
                {
                    if (propertyNames[i].AsView() == kLanes[k])
                    {
                        lane = static_cast<isize>(k);
                    }
                }
                if (lane >= 0)
                {
                    const u32 offset = i < propertyOffsets.Size() ? propertyOffsets[i] : ~0u;
                    if (offset != ~0u && static_cast<usize>(offset) + sizeof(f32) <= uniformDefaults.Size())
                    {
                        MemCopy(&values[lane], uniformDefaults.Data() + offset, sizeof(f32));
                    }
                    continue;
                }
                if (w != i)
                {
                    propertyNames[w] = Move(propertyNames[i]);
                    if (i < propertyTypes.Size())
                    {
                        propertyTypes[w] = propertyTypes[i];
                    }
                    if (i < propertyOffsets.Size())
                    {
                        propertyOffsets[w] = propertyOffsets[i];
                    }
                    if (i < propertySizes.Size())
                    {
                        propertySizes[w] = propertySizes[i];
                    }
                }
                if (w < propertyBindings.Size())
                {
                    propertyBindings[w] = static_cast<u32>(w); // a builtin's ordinals are its order
                }
                ++w;
            }
            propertyNames.Resize(w);
            propertyTypes.Resize(Min(w, propertyTypes.Size()));
            propertyOffsets.Resize(Min(w, propertyOffsets.Size()));
            propertySizes.Resize(Min(w, propertySizes.Size()));
            propertyBindings.Resize(Min(w, propertyBindings.Size()));

            for (const MaterialEffectGroupSource& group : effects)
            {
                if (group.name.AsView() == u8"Wind")
                {
                    return; // already carried
                }
            }
            MaterialEffectGroupSource wind;
            wind.name = String(u8"Wind");
            wind.flag = static_cast<u32>(shaders::ShaderFlags::Wind);
            wind.gateIndex = 0;
            wind.uniformSize = 16;
            wind.values.Resize(16);
            for (u8& b : wind.values)
            {
                b = 0;
            }
            for (usize k = 0; k < 3; ++k)
            {
                wind.propertyNames.PushBack(String(kParameters[k]));
                wind.propertyTypes.PushBack(static_cast<u8>(MaterialPropertyType::Float));
                wind.propertyOffsets.PushBack(static_cast<u32>(k * sizeof(f32)));
                wind.propertySizes.PushBack(static_cast<u32>(sizeof(f32)));
                MemCopy(wind.values.Data() + k * sizeof(f32), &values[k], sizeof(f32));
            }
            effects.PushBack(Move(wind));
        }

        // A material stored before materials had colour properties declared its colours as
        // Float4. A Float4 that the builtin template of this material's shader declares as a
        // colour takes the template's colour type, so its value is treated as the sRGB colour it
        // was entered as (and the editor shows a colour picker for it). Idempotent; a custom
        // shader's properties are left as they are.
        void AdoptTemplateColorTypes()
        {
            RefPtr<Material> templ = BuiltinMaterialTemplate(shaderName.AsView());
            if (!templ)
            {
                return;
            }
            const usize count = Min(propertyNames.Size(), propertyTypes.Size());
            for (usize i = 0; i < count; ++i)
            {
                if (propertyTypes[i] != static_cast<u8>(MaterialPropertyType::Float4))
                {
                    continue;
                }
                const MaterialPropertyDef* def = templ->FindProperty(propertyNames[i].AsView());
                if (def != nullptr && def->IsColor())
                {
                    propertyTypes[i] = static_cast<u8>(def->type);
                }
            }
        }

        // Captures a built Material's declared layout + defaults into an authorable source
        // referencing `shaderId`. (Used by the editor cook and round-trip tests.)
        static void FromMaterial(const Material& material, const Guid& shaderId,
                                 MaterialSource& out)
        {
            out.name = String(material.name.AsView());
            out.shaderId = shaderId;
            out.shaderName = String(material.shaderName.AsView());
            out.shaderFlags = static_cast<u32>(material.shaderFlags);
            out.blendMode = material.pipeline.blendMode;
            out.depthMode = material.pipeline.depthMode;
            out.cullMode = material.pipeline.cullMode;
            out.vertexLayout = material.pipeline.vertexLayout;
            out.samplerU = static_cast<u8>(material.samplerU);
            out.samplerV = static_cast<u8>(material.samplerV);

            out.propertyNames.Clear();
            out.propertyTypes.Clear();
            out.propertyBindings.Clear();
            out.propertyOffsets.Clear();
            out.propertySizes.Clear();
            for (const MaterialPropertyDef& d : material.Properties())
            {
                out.propertyNames.PushBack(String(d.name));
                out.propertyTypes.PushBack(static_cast<u8>(d.type));
                out.propertyBindings.PushBack(d.binding);
                out.propertyOffsets.PushBack(d.offset);
                out.propertySizes.PushBack(d.size);
            }
            const Span<const u8> defaults = material.DefaultUniformData();
            out.uniformDefaults.Clear();
            out.uniformDefaults.Reserve(defaults.Size());
            for (usize i = 0; i < defaults.Size(); ++i)
            {
                out.uniformDefaults.PushBack(defaults[i]);
            }
            out.effects.Clear();
            for (const MaterialEffectGroup& group : material.EffectGroups())
            {
                out.effects.PushBack(EffectGroupSourceOf(group));
            }
        }

        // A material's effect group as stored.
        [[nodiscard]] static MaterialEffectGroupSource EffectGroupSourceOf(const MaterialEffectGroup& group)
        {
            MaterialEffectGroupSource g;
            g.name = String(group.name);
            g.flag = static_cast<u32>(group.flag);
            g.gateIndex = group.gateIndex;
            g.uniformSize = group.uniformSize;
            for (const MaterialPropertyDef& d : group.properties)
            {
                g.propertyNames.PushBack(String(d.name));
                g.propertyTypes.PushBack(static_cast<u8>(d.type));
                g.propertyOffsets.PushBack(d.offset);
                g.propertySizes.PushBack(d.size);
            }
            g.values = group.defaults;
            return g;
        }
    };

    // Builds a MaterialSource into a runtime Material, binding the referenced shader
    // resource (which records the dependency edge) to resolve its name.
    // A "forward" material source must carry the forward shader's full set-2 property table
    // (EmissiveColor @32, OcclusionStrength/NormalScale/AlphaCutoff @48/52/56): without them the
    // uniform buffer is short and the shader reads past it. Sources are never upgraded in
    // memory - a source missing one is stale and is refused (re-create the material). New
    // CreatePBR sources carry them; unlit and custom shaders are untouched.
    [[nodiscard]] inline bool ForwardMaterialSourceIsComplete(const MaterialSource& src,
                                                              StringView* outMissing = nullptr)
    {
        if (src.shaderName != u8"forward")
        {
            return true;
        }
        static constexpr StringView kRequired[] = {u8"EmissiveColor", u8"OcclusionStrength",
                                                   u8"NormalScale", u8"AlphaCutoff"};
        for (StringView required : kRequired)
        {
            bool present = false;
            for (const String& n : src.propertyNames)
            {
                if (n.AsView() == required)
                {
                    present = true;
                    break;
                }
            }
            if (!present)
            {
                if (outMissing != nullptr)
                {
                    *outMissing = required;
                }
                return false;
            }
        }
        return true;
    }

    /// A runtime material from a cooked source, drawn with `shaderName`: its property layout,
    /// default values, render state and sampler addressing; its textures are left unbound (the
    /// factory resolves them through the resource manager, an editor preview through its own).
    /// The one conversion, so a preview draws what a loaded material draws.
    [[nodiscard]] inline RefPtr<Material> MaterialFromSource(IAllocator& allocator,
                                                             const MaterialSource& src,
                                                             StringView shaderName)
    {
        RefPtr<Material> material = MakeRef<Material>(allocator);
        material->name = String(src.name.AsView());
        material->shaderName = String(shaderName);
        material->shaderFlags = static_cast<shaders::ShaderFlags>(src.shaderFlags);

        const usize count = src.propertyNames.Size();
        for (usize i = 0; i < count; ++i)
        {
            MaterialPropertyDef d{};
            d.name = src.propertyNames[i].AsView();
            d.type = static_cast<MaterialPropertyType>(src.propertyTypes[i]);
            d.binding = (i < src.propertyBindings.Size()) ? src.propertyBindings[i] : 0u;
            d.offset = (i < src.propertyOffsets.Size()) ? src.propertyOffsets[i] : 0u;
            d.size = (i < src.propertySizes.Size()) ? src.propertySizes[i] : 0u;
            material->AddProperty(d);
        }
        material->AllocateDefaultUniformData();
        material->SetRawDefaultUniformData(
            Span<const u8>{src.uniformDefaults.Data(), src.uniformDefaults.Size()});
        // The effect groups, each as stored (the material needs nothing else to draw them).
        for (const MaterialEffectGroupSource& group : src.effects)
        {
            Array<MaterialPropertyDef> defs;
            const usize n = group.propertyNames.Size();
            for (usize i = 0; i < n; ++i)
            {
                MaterialPropertyDef d{};
                d.name = group.propertyNames[i].AsView();
                d.type = i < group.propertyTypes.Size() ? static_cast<MaterialPropertyType>(group.propertyTypes[i])
                                                        : MaterialPropertyType::Float;
                d.binding = static_cast<u32>(i);
                d.offset = i < group.propertyOffsets.Size() ? group.propertyOffsets[i] : 0u;
                d.size = i < group.propertySizes.Size() ? group.propertySizes[i] : 0u;
                defs.PushBack(d);
            }
            material->SetEffectGroup(group.name.AsView(), static_cast<shaders::ShaderFlags>(group.flag),
                                     group.gateIndex, group.uniformSize,
                                     Span<const MaterialPropertyDef>{defs.Data(), defs.Size()},
                                     Span<const u8>{group.values.Data(), group.values.Size()});
        }

        material->pipeline = PipelineConfig{};
        material->pipeline.shaderName = material->shaderName.AsView();
        material->pipeline.shaderFlags = material->shaderFlags;
        material->pipeline.blendMode = src.blendMode;
        material->pipeline.depthMode = src.depthMode;
        material->pipeline.cullMode = src.cullMode;
        material->pipeline.vertexLayout = src.vertexLayout;
        material->samplerU = static_cast<rhi::AddressMode>(src.samplerU);
        material->samplerV = static_cast<rhi::AddressMode>(src.samplerV);
        return material;
    }

    class MaterialFactory final : public IResourceFactory
    {
    public:
        MaterialFactory() noexcept : m_allocator(&DefaultAllocator()) {}
        // The allocator backs every material it creates. With the shader system (borrowed; may be
        // null): the factory re-lays each material's effect groups on the effects' live tables, by
        // what its shader's vertex stage declares (a material saved before an effect existed
        // gains its group; one whose shader no longer declares the effect loses it). Without one,
        // a material keeps the groups its source carries, which is all it needs to draw them.
        MaterialFactory(IAllocator& allocator, shaders::ShaderSystem* system) noexcept
            : m_allocator(&allocator), m_system(system)
        {
        }

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &Material::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &MaterialSource::StaticType();
        }

        [[nodiscard]] RefPtr<Object> Create(ResourceManager& manager,
                                            foundation::content::Instance& instance) override
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            MaterialSource* src = Cast<MaterialSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            StringView missing;
            if (!ForwardMaterialSourceIsComplete(*src, &missing))
            {
                LOG_ERROR(u8"Materials",
                          u8"material '{}' is missing the forward shader property '{}' (a stale "
                          u8"source) - re-create the material",
                          src->name, missing);
                return RefPtr<Object>{};
            }

            // Resolve the shader: a cooked ShaderResource (by id, recording the material->shader edge)
            // or a builtin shader named directly (shaderName) when no resource id is given.
            String shaderName;
            String depthName;
            String pickName;
            // Where the material's effects are declared: the shader's vertex variants, and the
            // shader system serving the effects' tables (a custom shader's own, else the host's).
            shaders::ShaderSystem* effectSystem = nullptr;
            shaders::ShaderFlags vertexVariants = shaders::ShaderFlags::None;
            if (!src->shaderId.IsNil())
            {
                Proxy<shaders::ShaderResource> shader =
                    manager.Bind<shaders::ShaderResource>(src->shaderId);
                if (shader)
                {
                    shaderName = String(shader->Name());
                    // Its own depth and pick programs, where it needs them.
                    const bool masked = src->blendMode == BlendMode::Masked;
                    depthName = String(shader->DepthProgram(masked));
                    pickName = String(shader->PickProgram(masked));
                    effectSystem = shader->System();
                    vertexVariants = shader->VertexVariants();
                }
            }
            if (shaderName.IsEmpty())
            {
                shaderName = String(src->shaderName.AsView());
                if (m_system != nullptr && !shaderName.IsEmpty())
                {
                    effectSystem = m_system;
                    vertexVariants = m_system->DeclaredVariants(shaderName.AsView(), shaders::ShaderStage::Vertex);
                }
            }

            RefPtr<Material> material = MaterialFromSource(*m_allocator, *src, shaderName.AsView());
            material->depthShaderName = Move(depthName);
            material->pickShaderName = Move(pickName);
            // An empty library says nothing (a shader system with no corpus): the stored groups stand.
            if (effectSystem != nullptr && !effectSystem->Effects().IsEmpty())
            {
                material->AdoptDeclaredEffects(effectSystem->Effects().All(), vertexVariants);
            }

            // Default texture bindings: resolve each through the manager (the Bind records the
            // material->texture edge, so a texture reload rebuilds this material) and install as
            // the material's default. Missing textures (not cooked yet, no GPU factory in
            // headless tools) just leave the slot unbound.
            for (usize i = 0; i < src->textureSlots.Size() && i < src->textureIds.Size(); ++i)
            {
                if (src->textureIds[i].IsNil())
                {
                    continue;
                }
                // Route by the async flag (the scope the scene resolve sets): a direct
                // manager.Bind never consults it, and THIS loop was the serial-load stall -
                // 69 Sponza textures decoded on the UI thread inside material builds.
                Proxy<texture::Texture> tex =
                    manager.AsyncBindsEnabled()
                        ? manager.BindAsync<texture::Texture>(src->textureIds[i])
                        : manager.Bind<texture::Texture>(src->textureIds[i]);
                if (!tex)
                {
                    if (tex.Handle() != nullptr &&
                        tex.Handle()->State() == ResourceState::Pending)
                    {
                        // Decoding on a worker: the slot stays unbound QUIETLY - the settle
                        // reloads this material through the recorded edge and fills it.
                        continue;
                    }
                    LOG_WARNING(u8"Materials",
                                         u8"material '{}': texture for slot '{}' failed to bind "
                                         u8"(no product / no texture factory?)",
                                         src->name, src->textureSlots[i]);
                    continue;
                }
                if (tex->View() == nullptr)
                {
                    LOG_WARNING(u8"Materials",
                                         u8"material '{}': texture for slot '{}' has no GPU view",
                                         src->name, src->textureSlots[i]);
                    continue;
                }
                if (material->FindProperty(src->textureSlots[i].AsView()) == nullptr)
                {
                    LOG_WARNING(
                        u8"Materials",
                        u8"material '{}': no texture property named '{}' in its layout", src->name,
                        src->textureSlots[i]);
                    continue;
                }
                material->SetDefaultTexture(src->textureSlots[i].AsView(), tex->View());
                LOG_DEBUG(u8"Materials", u8"material '{}': slot '{}' bound", src->name,
                                   src->textureSlots[i]);
            }
            return material;
        }

    private:
        IAllocator* m_allocator;
        shaders::ShaderSystem* m_system = nullptr; // borrowed; null in a host without one
    };

    // MaterialSource::StaticType() is defined WITH its reflected properties + data version (4)
    // in MaterialResourceImpl.cpp (GCC module hygiene: REFLECT_MEMBERS bodies out of interfaces).

} // namespace foundation::materials

export namespace foundation::materials
{
    /// Registers the cooked record + product (content-DB construction by type name). Idempotent.
    inline void RegisterMaterialResourceTypes()
    {
        GlobalTypeRegistry().Register(MaterialSource::StaticType());
        RegisterSerializable<MaterialSource>();
        GlobalTypeRegistry().Register(Material::StaticType());
    }

    /// The materials resource module (engine-composition.md D1): the module the engine
    /// composition composes this library's factories from.
    /// The material factory is created in every host and given the host's shader system when it
    /// has one (the effects' tables; MaterialFactory), not gated on it.
    inline constexpr foundation::resource::ResourceFactoryDesc kMaterialsResourceFactories[] = {
        foundation::resource::FactoryWithOptionalService<Material, MaterialSource, MaterialFactory,
                                                         shaders::ShaderSystem>(),
    };
    inline constexpr foundation::resource::ResourceModule kMaterialsResourceModule{
        u8"materials", &RegisterMaterialResourceTypes, kMaterialsResourceFactories,
        sizeof(kMaterialsResourceFactories) / sizeof(kMaterialsResourceFactories[0])};
}
