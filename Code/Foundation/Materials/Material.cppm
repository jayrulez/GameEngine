// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Materials - the `:material` partition.
///
/// Material: the shared, immutable template - a shader name + variant flags, a list
/// of declared properties, a PipelineConfig, and default values. It is *data*: the
/// MaterialSystem reads the property list to infer the GPU bind-group layout, so a
/// custom material/shader needs no renderer changes. Per-use overrides live in a
/// MaterialInstance. Material is an Object so it can be a resource-system product.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.materials:material;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import :types;
import :pipeline;

using namespace foundation::core;
namespace shaders = foundation::shaders;
namespace rhi = foundation::rhi;

export namespace foundation::materials
{

    inline constexpr u32 kNoMaterialEffectGate = ~0u;

    // The material kind of an effect's or a custom shader's property type: the one mapping from
    // the shader tables (foundation.shaders) to a material's declared types.
    [[nodiscard]] constexpr MaterialPropertyType MaterialPropertyTypeOf(shaders::ShaderPropertyType type) noexcept
    {
        using T = shaders::ShaderPropertyType;
        switch (type)
        {
        case T::Float: return MaterialPropertyType::Float;
        case T::Float2: return MaterialPropertyType::Float2;
        case T::Float3: return MaterialPropertyType::Float3;
        case T::Float4: return MaterialPropertyType::Float4;
        case T::Int: return MaterialPropertyType::Int;
        case T::Color: return MaterialPropertyType::Color;
        case T::ColorHdr: return MaterialPropertyType::ColorHdr;
        case T::Texture: return MaterialPropertyType::Texture2D;
        case T::TextureCube: return MaterialPropertyType::TextureCube;
        case T::Sampler: return MaterialPropertyType::Sampler;
        }
        return MaterialPropertyType::Float;
    }

    // One effect's parameters on a material (Specs/wind-for-every-shader.md): a group of its own,
    // apart from the shader's properties and never in the shader's buffer. A material whose shader
    // declares an effect's variant flag carries the effect's group: its parameters' names, types
    // and offsets in the effect's own buffer (the effect block's table), and their values. The
    // effect is active while its gate is above zero, and an active effect turns its flag on; what
    // the effect does is its shader code's, so nothing here knows wind. Scripts and overrides
    // address a value as `Effect.Property` (`Wind.Strength`).
    struct MaterialEffectGroup
    {
        StringView name; // the effect's (`Wind`); a view into the owning material's name backing
        shaders::ShaderFlags flag = shaders::ShaderFlags::None;
        u32 gateIndex = kNoMaterialEffectGate; // index into `properties`
        u32 uniformSize = 0;                   // the effect buffer's size (a whole row)
        Array<MaterialPropertyDef> properties; // offsets in the effect's buffer; names viewed as `name`
        Array<u8> defaults;                    // `uniformSize` bytes: the material's values

        [[nodiscard]] isize GetPropertyIndex(StringView propertyName) const noexcept
        {
            for (usize i = 0; i < properties.Size(); ++i)
            {
                if (properties[i].name == propertyName)
                {
                    return static_cast<isize>(i);
                }
            }
            return -1;
        }
        [[nodiscard]] const MaterialPropertyDef* FindProperty(StringView propertyName) const noexcept
        {
            const isize i = GetPropertyIndex(propertyName);
            return i >= 0 ? &properties[static_cast<usize>(i)] : nullptr;
        }
        [[nodiscard]] const MaterialPropertyDef* Gate() const noexcept
        {
            return gateIndex < properties.Size() ? &properties[gateIndex] : nullptr;
        }
        // Whether the effect is on for these values (the material's defaults or an instance's):
        // its float gate above zero.
        [[nodiscard]] bool IsActive(Span<const u8> values) const noexcept
        {
            const MaterialPropertyDef* gate = Gate();
            if (gate == nullptr || gate->type != MaterialPropertyType::Float ||
                static_cast<usize>(gate->offset) + sizeof(f32) > values.Size())
            {
                return false;
            }
            f32 value = 0.0f;
            MemCopy(&value, values.Data() + gate->offset, sizeof(f32));
            return value > 0.0f;
        }
        [[nodiscard]] bool IsActive() const noexcept
        {
            return IsActive(Span<const u8>{defaults.Data(), defaults.Size()});
        }
    };

    // Splits `Effect.Property` at its dot. False for a name without one (a shader property's:
    // HLSL identifiers have no dot, so the dot alone says which set a name addresses).
    [[nodiscard]] inline bool SplitEffectPropertyName(StringView qualified, StringView& outEffect,
                                                      StringView& outProperty) noexcept
    {
        for (usize i = 0; i < qualified.Size(); ++i)
        {
            if (qualified[i] == u8'.')
            {
                outEffect = qualified.SubStr(0, i);
                outProperty = qualified.SubStr(i + 1, qualified.Size() - i - 1);
                return true;
            }
        }
        return false;
    }

    // Shared material template. Properties are declared once; defaults seed every
    // instance. Name strings are owned in a stable backing so property views stay valid.
    class Material final : public Object
    {
        RTTI_OBJECT(Material, Object)
    public:
        // Unique per-OBJECT id: renderer caches key by THIS, never by pointer (a reloaded
        // material can reallocate at the freed address - the bind-group versioning rule).
        const u64 uid = NextUid();

        // Sampler address modes for the material's texture slots (the importer wires them from
        // the source asset's sampler; glTF's default is Repeat). The MaterialSystem resolves the
        // actual rhi::Sampler from these when building the instance's bind group.
        rhi::AddressMode samplerU = rhi::AddressMode::Repeat;
        rhi::AddressMode samplerV = rhi::AddressMode::Repeat;

        String name;
        String shaderName;
        // The shader's own programs for the depth-like passes (the camera depth prepass, the shadow
        // maps, the editor's pick), when it has them (a surface shader that moves its vertices or
        // cuts out with its own alpha); empty = the built-in `shadow_depth` and `pick_ids`.
        String depthShaderName;
        String pickShaderName;
        shaders::ShaderFlags shaderFlags = shaders::ShaderFlags::None;
        PipelineConfig pipeline;

        [[nodiscard]] bool IsValid() const noexcept { return shaderName.Size() > 0; }
        [[nodiscard]] usize PropertyCount() const noexcept { return m_properties.Size(); }
        [[nodiscard]] u32 UniformDataSize() const noexcept { return m_uniformDataSize; }

        [[nodiscard]] const MaterialPropertyDef& GetProperty(usize index) const noexcept
        {
            return m_properties[index];
        }

        // Index of the named property, or -1.
        [[nodiscard]] isize GetPropertyIndex(StringView n) const noexcept
        {
            for (usize i = 0; i < m_properties.Size(); ++i)
            {
                if (m_properties[i].name == n)
                {
                    return static_cast<isize>(i);
                }
            }
            return -1;
        }

        [[nodiscard]] const MaterialPropertyDef* FindProperty(StringView n) const noexcept
        {
            const isize i = GetPropertyIndex(n);
            return (i >= 0) ? &m_properties[static_cast<usize>(i)] : nullptr;
        }

        [[nodiscard]] Span<const MaterialPropertyDef> Properties() const noexcept
        {
            return {m_properties.Data(), m_properties.Size()};
        }

        // Declares a property. The name is cloned into stable backing so the caller's
        // string need not outlive the material; uniform size grows to fit uniforms.
        void AddProperty(const MaterialPropertyDef& prop)
        {
            MaterialPropertyDef d = prop;
            d.name = OwnName(prop.name);
            m_properties.PushBack(d);

            if (d.IsUniform())
            {
                const u32 end = d.offset + d.size;
                if (end > m_uniformDataSize)
                {
                    // Round to cbuffer alignment: the shader-side struct is 16-byte
                    // padded on every API, and WebGPU VALIDATES the bound range
                    // against the struct size (a 60-byte range under a 64-byte
                    // cbuffer fails). Buffer and binding both use this size.
                    m_uniformDataSize = (end + 15u) & ~15u;
                }
            }
        }

        // (Re)allocates the default uniform buffer, preserving existing defaults.
        void AllocateDefaultUniformData()
        {
            if (m_uniformDataSize == 0)
            {
                return;
            }
            if (m_defaultUniformData.Size() < m_uniformDataSize)
            {
                Array<u8> grown;
                grown.Resize(m_uniformDataSize);
                for (usize i = 0; i < m_defaultUniformData.Size(); ++i)
                {
                    grown[i] = m_defaultUniformData[i];
                }
                m_defaultUniformData = Move(grown);
            }
        }

        void SetDefaultFloat(StringView n, f32 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultFloat2(StringView n, Float2 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultFloat3(StringView n, Float3 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultFloat4(StringView n, Float4 v) { WriteUniform(n, &v, sizeof(v)); }
        void SetDefaultColor(StringView n, Float4 c) { SetDefaultFloat4(n, c); }

        void SetDefaultTexture(StringView n, rhi::TextureView* tex)
        {
            const isize i = GetPropertyIndex(n);
            if (i >= 0 && m_properties[static_cast<usize>(i)].IsTexture())
            {
                m_defaultTextures.InsertOrAssign(static_cast<usize>(i), tex);
            }
        }
        void SetDefaultSampler(StringView n, rhi::Sampler* s)
        {
            const isize i = GetPropertyIndex(n);
            if (i >= 0 && m_properties[static_cast<usize>(i)].IsSampler())
            {
                m_defaultSamplers.InsertOrAssign(static_cast<usize>(i), s);
            }
        }

        [[nodiscard]] Span<const u8> DefaultUniformData() const noexcept
        {
            return {m_defaultUniformData.Data(), m_defaultUniformData.Size()};
        }

        // Overlays a raw default-uniform blob (the cooked/serialized defaults) over the
        // uniform buffer. Used by the resource factory to restore authored defaults.
        void SetRawDefaultUniformData(Span<const u8> data)
        {
            AllocateDefaultUniformData();
            const usize n = data.Size() < m_defaultUniformData.Size() ? data.Size()
                                                                      : m_defaultUniformData.Size();
            if (n > 0)
            {
                MemCopy(m_defaultUniformData.Data(), data.Data(), n);
            }
        }
        [[nodiscard]] rhi::TextureView* GetDefaultTexture(usize propIndex) const noexcept
        {
            rhi::TextureView* const* p = m_defaultTextures.Find(propIndex);
            return (p != nullptr) ? *p : nullptr;
        }
        [[nodiscard]] rhi::Sampler* GetDefaultSampler(usize propIndex) const noexcept
        {
            rhi::Sampler* const* p = m_defaultSamplers.Find(propIndex);
            return (p != nullptr) ? *p : nullptr;
        }

        // --- effect groups (Specs/wind-for-every-shader.md) ---

        [[nodiscard]] usize EffectGroupCount() const noexcept { return m_effectGroups.Size(); }
        [[nodiscard]] const MaterialEffectGroup& GetEffectGroup(usize index) const noexcept
        {
            return m_effectGroups[index];
        }
        [[nodiscard]] Span<const MaterialEffectGroup> EffectGroups() const noexcept
        {
            return {m_effectGroups.Data(), m_effectGroups.Size()};
        }
        // Index of the named effect's group, or -1.
        [[nodiscard]] isize FindEffectGroup(StringView effectName) const noexcept
        {
            for (usize i = 0; i < m_effectGroups.Size(); ++i)
            {
                if (m_effectGroups[i].name == effectName)
                {
                    return static_cast<isize>(i);
                }
            }
            return -1;
        }
        // The parameter `Effect.Property` names, and its group's index; null when the material
        // carries no such group or the group no such parameter.
        [[nodiscard]] const MaterialPropertyDef* FindEffectProperty(StringView qualified,
                                                                    usize* outGroup = nullptr) const noexcept
        {
            StringView effect;
            StringView property;
            if (!SplitEffectPropertyName(qualified, effect, property))
            {
                return nullptr;
            }
            const isize g = FindEffectGroup(effect);
            if (g < 0)
            {
                return nullptr;
            }
            const MaterialPropertyDef* def = m_effectGroups[static_cast<usize>(g)].FindProperty(property);
            if (def != nullptr && outGroup != nullptr)
            {
                *outGroup = static_cast<usize>(g);
            }
            return def;
        }

        // Adds an effect's group (replacing one of the same name): its parameters' names, types
        // and offsets in the effect's buffer, the gate's index and the buffer's size, and the
        // values (`uniformSize` bytes; shorter is zero-filled). What a material from an asset
        // restores from its source.
        void SetEffectGroup(StringView effectName, shaders::ShaderFlags flag, u32 gateIndex,
                            u32 uniformSize, Span<const MaterialPropertyDef> properties,
                            Span<const u8> values)
        {
            MaterialEffectGroup group;
            group.name = OwnName(effectName);
            group.flag = flag;
            group.gateIndex = gateIndex;
            group.uniformSize = CbufferRowSize(uniformSize);
            for (const MaterialPropertyDef& p : properties)
            {
                MaterialPropertyDef d = p;
                d.name = OwnName(p.name);
                group.properties.PushBack(d);
                group.uniformSize = Max(group.uniformSize, CbufferRowSize(d.offset + d.size));
            }
            group.defaults.Resize(group.uniformSize);
            for (u8& b : group.defaults)
            {
                b = 0;
            }
            const usize n = Min(values.Size(), group.defaults.Size());
            if (n > 0)
            {
                MemCopy(group.defaults.Data(), values.Data(), n);
            }
            const isize existing = FindEffectGroup(effectName);
            if (existing >= 0)
            {
                m_effectGroups[static_cast<usize>(existing)] = Move(group);
            }
            else
            {
                m_effectGroups.PushBack(Move(group));
            }
        }

        // Adds an effect's group from the effect's table (the shader system's, from the effect
        // block), at the effect's defaults: what a material built in code takes from whoever
        // holds the shader system. An effect takes uniforms only, so every parameter is one.
        void AddEffectGroup(const shaders::ShaderEffect& effect)
        {
            Array<MaterialPropertyDef> defs;
            Array<u8> values;
            values.Resize(effect.uniformSize);
            for (u8& b : values)
            {
                b = 0;
            }
            for (usize i = 0; i < effect.properties.Size(); ++i)
            {
                const shaders::ShaderProperty& p = effect.properties[i];
                MaterialPropertyDef d{};
                d.name = p.name.AsView();
                d.type = MaterialPropertyTypeOf(p.type);
                d.binding = static_cast<u32>(i);
                d.offset = p.offset;
                d.size = p.size;
                defs.PushBack(d);
                if (p.size == 0u || static_cast<usize>(p.offset) + p.size > values.Size())
                {
                    continue;
                }
                if (p.type == shaders::ShaderPropertyType::Int)
                {
                    const i32 value = static_cast<i32>(p.value.x); // an int as the shader reads it
                    MemCopy(values.Data() + p.offset, &value, sizeof(value));
                }
                else
                {
                    const f32 components[4] = {p.value.x, p.value.y, p.value.z, p.value.w};
                    MemCopy(values.Data() + p.offset, components, Min<usize>(p.size, sizeof(components)));
                }
            }
            SetEffectGroup(effect.name.AsView(), effect.flag, effect.gateIndex, effect.uniformSize,
                           Span<const MaterialPropertyDef>{defs.Data(), defs.Size()},
                           Span<const u8>{values.Data(), values.Size()});
        }

        // Adds the group of every effect in `effects` whose flag `vertexVariants` declares (the
        // variants line of the material's shader's vertex stage), and drops any group whose
        // effect the shader no longer declares. A group already carried keeps the values whose
        // name, type and size still fit (an effect's table may have grown since it was saved).
        void AdoptDeclaredEffects(Span<const shaders::ShaderEffect> effects, shaders::ShaderFlags vertexVariants)
        {
            Array<MaterialEffectGroup> kept;
            for (const shaders::ShaderEffect& effect : effects)
            {
                if (!shaders::HasFlag(vertexVariants, effect.flag))
                {
                    continue;
                }
                const isize old = FindEffectGroup(effect.name.AsView());
                MaterialEffectGroup previous;
                if (old >= 0)
                {
                    previous = Move(m_effectGroups[static_cast<usize>(old)]);
                    m_effectGroups.RemoveAt(static_cast<usize>(old));
                }
                AddEffectGroup(effect);
                MaterialEffectGroup fresh = Move(m_effectGroups[m_effectGroups.Size() - 1]);
                m_effectGroups.PopBack();
                for (const MaterialPropertyDef& d : fresh.properties)
                {
                    const MaterialPropertyDef* was = previous.FindProperty(d.name);
                    if (was != nullptr && was->type == d.type && was->size == d.size &&
                        static_cast<usize>(was->offset) + was->size <= previous.defaults.Size() &&
                        static_cast<usize>(d.offset) + d.size <= fresh.defaults.Size())
                    {
                        MemCopy(fresh.defaults.Data() + d.offset, previous.defaults.Data() + was->offset, d.size);
                    }
                }
                kept.PushBack(Move(fresh));
            }
            m_effectGroups = Move(kept);
        }

        void ClearEffectGroups() { m_effectGroups.Clear(); }

    private:
        // A copy of `n` in the stable name backing; the view stays valid for the material's life.
        StringView OwnName(StringView n)
        {
            UniquePtr<String> owned = MakeUnique<String>(DefaultAllocator(), n);
            const StringView view = owned->AsView();
            m_propertyNames.PushBack(Move(owned));
            return view;
        }
        [[nodiscard]] static u32 CbufferRowSize(u32 bytes) noexcept { return (bytes + 15u) & ~15u; }

        void WriteUniform(StringView n, const void* src, usize bytes)
        {
            usize group = 0;
            if (const MaterialPropertyDef* e = FindEffectProperty(n, &group))
            {
                Array<u8>& values = m_effectGroups[group].defaults;
                if (bytes <= e->size && static_cast<usize>(e->offset) + bytes <= values.Size())
                {
                    MemCopy(values.Data() + e->offset, src, bytes);
                }
                return;
            }
            const isize i = GetPropertyIndex(n);
            if (i < 0)
            {
                return;
            }
            const MaterialPropertyDef& d = m_properties[static_cast<usize>(i)];
            if (!d.IsUniform() || d.offset + bytes > m_defaultUniformData.Size())
            {
                return;
            }
            MemCopy(m_defaultUniformData.Data() + d.offset, src, bytes);
        }

        Array<MaterialPropertyDef> m_properties;
        Array<UniquePtr<String>> m_propertyNames; // stable backing for property and effect name views
        Array<MaterialEffectGroup> m_effectGroups; // in the order the shader's effects were adopted
        Array<u8> m_defaultUniformData;
        u32 m_uniformDataSize = 0;
        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        HashMap<usize, rhi::TextureView*> m_defaultTextures;
        HashMap<usize, rhi::Sampler*> m_defaultSamplers;
    };

    RTTI_DEFINE_OBJECT(Material, "rtti::materials")

    // The uniform bytes a shader receives: the authored bytes as they are, except each colour
    // property, which is authored sRGB and decoded to linear here (a ColorHdr's rgb also scaled by
    // its intensity, its w set to 1). The one place a material's colours meet the GPU, like the
    // hardware decode of an sRGB texture. `gpu` holds at least `authored.Size()` bytes.
    // Over any declared set: the material's properties, or one of its effect groups'.
    inline void EncodeUniformsForGpu(Span<const MaterialPropertyDef> properties, Span<const u8> authored, u8* gpu)
    {
        if (authored.Size() > 0)
        {
            MemCopy(gpu, authored.Data(), authored.Size());
        }
        for (const MaterialPropertyDef& d : properties)
        {
            if (!d.IsColor() || static_cast<usize>(d.offset) + sizeof(Float4) > authored.Size())
            {
                continue;
            }
            Float4 c;
            MemCopy(&c, authored.Data() + d.offset, sizeof(c));
            const Color linear = ToLinear(Color{c.x, c.y, c.z, c.w});
            Float4 out{linear.r, linear.g, linear.b, linear.a};
            if (d.type == MaterialPropertyType::ColorHdr)
            {
                out = Float4{linear.r * c.w, linear.g * c.w, linear.b * c.w, 1.0f};
            }
            MemCopy(gpu + d.offset, &out, sizeof(out));
        }
    }
    inline void EncodeUniformsForGpu(const Material& material, Span<const u8> authored, u8* gpu)
    {
        EncodeUniformsForGpu(material.Properties(), authored, gpu);
    }

} // namespace foundation::materials
