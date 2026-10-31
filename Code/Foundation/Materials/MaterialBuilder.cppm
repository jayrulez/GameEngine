// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Materials - the `:builder` partition.
///
/// Fluent builder for authoring a Material in code: declare the shader, pipeline
/// state, and typed properties. The builder lays out the uniform buffer by HLSL cbuffer
/// packing and assigns binding ordinals. Build() hands back the finished Material (a RefPtr,
/// since Material is an Object).

module;
#include "Core/Prelude.h"

export module foundation.materials:builder;

import foundation.core;
import foundation.rhi;
import foundation.shaders;
import :types;
import :pipeline;
import :material;

using namespace foundation::core;
namespace core = foundation::core;
namespace shaders = foundation::shaders;
namespace core =
    foundation::core; // to name the packed types where builder methods (Float2/3/4) shadow them
namespace rhi = foundation::rhi;

export namespace foundation::materials
{

    class MaterialBuilder
    {
    public:
        explicit MaterialBuilder(StringView name)
        {
            m_material = MakeRef<Material>(DefaultAllocator());
            m_material->name = String(name);
            m_material->pipeline = PipelineConfig{};
        }

        MaterialBuilder& Shader(StringView shaderName)
        {
            m_material->shaderName = String(shaderName);
            m_material->pipeline.shaderName = m_material->shaderName.AsView();
            return *this;
        }
        MaterialBuilder& Flags(shaders::ShaderFlags flags)
        {
            m_material->shaderFlags = flags;
            m_material->pipeline.shaderFlags = flags;
            return *this;
        }
        MaterialBuilder& VertexLayout(VertexLayoutType layout)
        {
            m_material->pipeline.vertexLayout = layout;
            return *this;
        }
        MaterialBuilder& Blend(BlendMode mode)
        {
            m_material->pipeline.blendMode = mode;
            return *this;
        }
        MaterialBuilder& Depth(DepthMode mode)
        {
            m_material->pipeline.depthMode = mode;
            return *this;
        }
        MaterialBuilder& Cull(CullModeConfig mode)
        {
            m_material->pipeline.cullMode = mode;
            return *this;
        }
        MaterialBuilder& DoubleSided()
        {
            m_material->pipeline.cullMode = CullModeConfig::None;
            return *this;
        }
        MaterialBuilder& Transparent()
        {
            m_material->pipeline.blendMode = BlendMode::AlphaBlend;
            m_material->pipeline.depthMode = DepthMode::ReadOnly;
            return *this;
        }
        MaterialBuilder& Additive()
        {
            m_material->pipeline.blendMode = BlendMode::Additive;
            m_material->pipeline.depthMode = DepthMode::ReadOnly;
            return *this;
        }

        // --- uniform properties (laid out into the material uniform buffer) ---
        MaterialBuilder& Float(StringView name, f32 v = 0.0f)
        {
            AddUniform(name, MaterialPropertyType::Float, 4);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat(name, v);
            return *this;
        }
        MaterialBuilder& Float2(StringView name, core::Float2 v = {})
        {
            AddUniform(name, MaterialPropertyType::Float2, 8);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat2(name, v);
            return *this;
        }
        MaterialBuilder& Float3(StringView name, core::Float3 v = {})
        {
            AddUniform(name, MaterialPropertyType::Float3, 12);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat3(name, v);
            return *this;
        }
        MaterialBuilder& Float4(StringView name, core::Float4 v = {})
        {
            AddUniform(name, MaterialPropertyType::Float4, 16);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat4(name, v);
            return *this;
        }
        // An authored colour (sRGB rgba); the GPU sees it linear.
        MaterialBuilder& Color(StringView name, core::Float4 v = core::Float4{1, 1, 1, 1})
        {
            AddUniform(name, MaterialPropertyType::Color, 16);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat4(name, v);
            return *this;
        }
        // An authored colour with an intensity: sRGB rgb, linear intensity in w.
        MaterialBuilder& ColorHdr(StringView name, core::Float4 colorAndIntensity)
        {
            AddUniform(name, MaterialPropertyType::ColorHdr, 16);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat4(name, colorAndIntensity);
            return *this;
        }

        // --- resource properties (become bind-group entries) ---
        MaterialBuilder& Texture(StringView name, rhi::TextureView* def = nullptr)
        {
            m_material->AddProperty(
                MaterialPropertyDef{name, MaterialPropertyType::Texture2D, m_binding++, 0, 0});
            if (def != nullptr)
            {
                m_material->SetDefaultTexture(name, def);
            }
            return *this;
        }
        MaterialBuilder& TextureCube(StringView name, rhi::TextureView* def = nullptr)
        {
            m_material->AddProperty(
                MaterialPropertyDef{name, MaterialPropertyType::TextureCube, m_binding++, 0, 0});
            if (def != nullptr)
            {
                m_material->SetDefaultTexture(name, def);
            }
            return *this;
        }
        MaterialBuilder& Sampler(StringView name, rhi::Sampler* def = nullptr)
        {
            m_material->AddProperty(
                MaterialPropertyDef{name, MaterialPropertyType::Sampler, m_binding++, 0, 0});
            if (def != nullptr)
            {
                m_material->SetDefaultSampler(name, def);
            }
            return *this;
        }

        // --- effect groups ---
        // The group of an effect the material's shader declares (its variant flag in the shader's
        // vertex variants), at the effect's defaults, from the effect's table in the shader system
        // (ShaderSystem::FindEffect or EffectForFlag). There is no C++ copy of a table: a builder
        // without one makes a material without the group.
        MaterialBuilder& Effect(const shaders::ShaderEffect& effect)
        {
            m_material->AddEffectGroup(effect);
            return *this;
        }

        // Finishes and yields the material. The builder is empty afterwards.
        [[nodiscard]] RefPtr<Material> Build()
        {
            m_material->AllocateDefaultUniformData();
            return Move(m_material);
        }

    private:
        // Lays a uniform out by HLSL's cbuffer packing (shaders::PackCbufferMember, the rule a
        // properties block's generated cbuffer follows too), so the offsets are where the shader
        // reads: a float after a float3 shares its row, a float2 never straddles one.
        void AddUniform(StringView name, MaterialPropertyType type, u32 size)
        {
            const u32 offset = shaders::PackCbufferMember(m_uniformOffset, size);
            m_material->AddProperty(MaterialPropertyDef{name, type, m_binding, offset, size});
            ++m_binding;
        }

        RefPtr<Material> m_material;
        u32 m_uniformOffset = 0;
        u32 m_binding = 0;
    };

    // Standard PBR material: the BaseColor, Metallic and Roughness uniforms, the five PBR maps and
    // a sampler, in the order the forward pass's set 2 expects. The importer builds these and
    // assigns the maps; unset maps fall back to neutral defaults in the renderer.
    [[nodiscard]] inline RefPtr<Material> CreatePBR(StringView name,
                                                    Float4 baseColor = Float4{1, 1, 1, 1},
                                                    f32 metallic = 0.0f, f32 roughness = 0.5f,
                                                    StringView shaderName = u8"forward")
    {
        return MaterialBuilder(name)
            .Shader(shaderName)
            .VertexLayout(VertexLayoutType::Mesh)
            .Color(u8"BaseColor", baseColor)
            .Float(u8"Metallic", metallic)
            .Float(u8"Roughness", roughness)
            // Offsets 24 and 60 are padding (forward.ps.hlsl's `_materialPad*`): the cbuffer packs
            // EmissiveColor at 32 and stays 64 bytes. Wind is an effect with a buffer of its own
            // (the material's Wind group, MaterialBuilder::Effect).
            // Black = none; rgb x EmissiveMap, times the intensity in w (glow above white).
            .ColorHdr(u8"EmissiveColor", Float4{0, 0, 0, 1})
            .Float(u8"OcclusionStrength", 1.0f)
            .Float(u8"NormalScale", 1.0f)
            .Float(u8"AlphaCutoff", 0.5f)
            .Texture(u8"AlbedoMap")
            .Texture(u8"NormalMap")
            .Texture(u8"MetallicRoughnessMap")
            .Texture(u8"OcclusionMap")
            .Texture(u8"EmissiveMap")
            .Sampler(u8"MainSampler")
            .Build();
    }

    // The standard unlit material: albedo * BaseColor, no lighting (the "unlit" builtin shader).
    // It shares forward's vertex path, so it casts shadows and goes through the post stack normally.
    [[nodiscard]] inline RefPtr<Material> CreateUnlit(StringView name,
                                                      Float4 baseColor = Float4{1, 1, 1, 1},
                                                      StringView shaderName = u8"unlit")
    {
        return MaterialBuilder(name)
            .Shader(shaderName)
            .VertexLayout(VertexLayoutType::Mesh)
            .Color(u8"BaseColor", baseColor)
            .Texture(u8"AlbedoMap")
            .Sampler(u8"MainSampler")
            .Build();
    }

    // The standard Phong material (the "phong" builtin shader), for models authored with Phong or
    // Lambert. Each light is shaded Blinn-Phong: the diffuse (BaseColor x AlbedoMap) by N.L, and
    // the specular (SpecularColor x SpecularMap, black for none) as a highlight whose exponent is
    // Shininess; then the scene's diffuse ambient and the emission are added. Its colour, map,
    // emission, normal map and cutoff take the names PBR and Unlit give them, so a material moved
    // between built-ins keeps them. It shares forward's vertex path, so it casts shadows and goes
    // through the post stack normally. The order lays the cbuffer out in 64 bytes, as
    // phong.ps.hlsl declares it.
    [[nodiscard]] inline RefPtr<Material> CreatePhong(StringView name,
                                                      Float4 baseColor = Float4{1, 1, 1, 1},
                                                      Float4 specularColor = Float4{0, 0, 0, 1},
                                                      f32 shininess = 20.0f,
                                                      StringView shaderName = u8"phong")
    {
        return MaterialBuilder(name)
            .Shader(shaderName)
            .VertexLayout(VertexLayoutType::Mesh)
            .Color(u8"BaseColor", baseColor)
            .Color(u8"SpecularColor", specularColor)
            // Black = none; rgb x EmissiveMap, times the intensity in w (glow above white).
            .ColorHdr(u8"EmissiveColor", Float4{0, 0, 0, 1})
            .Float(u8"Shininess", shininess)
            .Float(u8"NormalScale", 1.0f)
            .Float(u8"AlphaCutoff", 0.5f)
            .Texture(u8"AlbedoMap")
            .Texture(u8"SpecularMap")
            .Texture(u8"NormalMap")
            .Texture(u8"EmissiveMap")
            .Sampler(u8"MainSampler")
            .Build();
    }

    // The builtin shaders a material may be drawn with, and how each is known: by its shader's
    // name (what a material source stores), a short name (what an agent types) and a label (what
    // the editor shows). Every place that offers or recognises a builtin reads this one list.
    enum class BuiltinShading : u8
    {
        Pbr,
        Unlit,
        Phong,
    };
    struct BuiltinShadingInfo
    {
        BuiltinShading shading;
        StringView shaderName; // the material source's shaderName
        StringView shortName;  // "pbr", "unlit", "phong"
        StringView label;      // "PBR", "Unlit", "Phong"
        // What the builtin's vertex stage declares (its `// variants` line, pinned by
        // Materials.Tests against the shader file): the effects a material on it may carry where
        // no shader system is at hand to ask (a material moved between shaders in the editor
        // keeps its Wind group when both declare WIND).
        shaders::ShaderFlags vertexVariants;
    };
    inline constexpr shaders::ShaderFlags kBuiltinVertexVariants =
        shaders::ShaderFlags::Skinned | shaders::ShaderFlags::Instanced | shaders::ShaderFlags::Wind;
    inline constexpr BuiltinShadingInfo kBuiltinShadings[] = {
        {BuiltinShading::Pbr, u8"forward", u8"pbr", u8"PBR", kBuiltinVertexVariants},
        {BuiltinShading::Unlit, u8"unlit", u8"unlit", u8"Unlit", kBuiltinVertexVariants},
        {BuiltinShading::Phong, u8"phong", u8"phong", u8"Phong", kBuiltinVertexVariants},
    };
    inline constexpr usize kBuiltinShadingCount = sizeof(kBuiltinShadings) / sizeof(kBuiltinShadings[0]);
    [[nodiscard]] inline const BuiltinShadingInfo& BuiltinShadingOf(BuiltinShading shading)
    {
        return kBuiltinShadings[static_cast<usize>(shading)];
    }
    /// The builtin whose shader is `shaderName`, or null (a custom shader's name).
    [[nodiscard]] inline const BuiltinShadingInfo* FindBuiltinShadingByShader(StringView shaderName)
    {
        for (const BuiltinShadingInfo& info : kBuiltinShadings)
            if (info.shaderName == shaderName)
                return &info;
        return nullptr;
    }
    /// The builtin an agent names ("pbr", "unlit", "phong"), or null.
    [[nodiscard]] inline const BuiltinShadingInfo* FindBuiltinShadingByShortName(StringView shortName)
    {
        for (const BuiltinShadingInfo& info : kBuiltinShadings)
            if (info.shortName == shortName)
                return &info;
        return nullptr;
    }
    /// A material on `shading`'s builtin, its defaults.
    [[nodiscard]] inline RefPtr<Material> CreateBuiltin(BuiltinShading shading, StringView name)
    {
        switch (shading)
        {
        case BuiltinShading::Unlit:
            return CreateUnlit(name);
        case BuiltinShading::Phong:
            return CreatePhong(name);
        case BuiltinShading::Pbr:
        default:
            return CreatePBR(name);
        }
    }

    // The builtin template a builtin shader's materials are made from, or null for a custom
    // shader. A stored material's property table is checked against it.
    [[nodiscard]] inline RefPtr<Material> BuiltinMaterialTemplate(StringView shaderName)
    {
        const BuiltinShadingInfo* builtin = FindBuiltinShadingByShader(shaderName);
        return builtin != nullptr ? CreateBuiltin(builtin->shading, u8"__template") : RefPtr<Material>{};
    }

} // namespace foundation::materials
