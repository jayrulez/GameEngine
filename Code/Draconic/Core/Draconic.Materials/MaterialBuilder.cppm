/// Draconic::Materials - the `:builder` partition.
///
/// Fluent builder for authoring a Material in code: declare the shader, pipeline
/// state, and typed properties; the builder lays out the uniform buffer (std140-ish
/// 16-byte alignment for float3/float4) and assigns binding ordinals. Build() hands
/// back the finished Material (RefPtr, since Material is an Object).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.materials:builder;

import draconic.foundation;
import draconic.rhi;
import draconic.shaders;
import :types;
import :pipeline;
import :material;

using namespace draconic::foundation;
namespace foundation =
    draconic::foundation; // to name the packed types where builder methods (Float2/3/4) shadow them
namespace rhi = draconic::rhi;

export namespace draconic::materials
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
            AddUniform(name, MaterialPropertyType::Float, 4, /*align16*/ false);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat(name, v);
            return *this;
        }
        MaterialBuilder& Float2(StringView name, foundation::Float2 v = {})
        {
            AddUniform(name, MaterialPropertyType::Float2, 8, false);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat2(name, v);
            return *this;
        }
        MaterialBuilder& Float3(StringView name, foundation::Float3 v = {})
        {
            AddUniform(name, MaterialPropertyType::Float3, 12,
                       /*align16*/ true); // float3 occupies 16 (std140)
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat3(name, v);
            return *this;
        }
        MaterialBuilder& Float4(StringView name, foundation::Float4 v = {})
        {
            AddUniform(name, MaterialPropertyType::Float4, 16, true);
            m_material->AllocateDefaultUniformData();
            m_material->SetDefaultFloat4(name, v);
            return *this;
        }
        MaterialBuilder& Color(StringView name, foundation::Float4 v = foundation::Float4{1, 1, 1, 1})
        {
            return Float4(name, v);
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

        // Finishes and yields the material. The builder is empty afterwards.
        [[nodiscard]] RefPtr<Material> Build()
        {
            m_material->AllocateDefaultUniformData();
            return Move(m_material);
        }

    private:
        void AddUniform(StringView name, MaterialPropertyType type, u32 size, bool align16)
        {
            if (align16)
            {
                m_uniformOffset = (m_uniformOffset + 15u) & ~15u;
            }
            m_material->AddProperty(
                MaterialPropertyDef{name, type, m_binding, m_uniformOffset, size});
            m_uniformOffset += align16 ? 16u : size; // float3/float4 consume a 16-byte slot
            ++m_binding;
        }

        RefPtr<Material> m_material;
        u32 m_uniformOffset = 0;
        u32 m_binding = 0;
    };

    // Standard PBR material (mirrors Sedulous.Materials::CreatePBR): the BaseColor/Metallic/Roughness
    // uniforms + the five PBR maps + a sampler, in the order the forward set-2 contract expects. The
    // importer builds these and assigns the maps; unset maps fall back to neutral defaults in the renderer.
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
            .Color(u8"EmissiveColor", Float4{0, 0, 0, 1}) // black = none (rgb x EmissiveMap)
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

    // The standard UNLIT material: albedo * BaseColor, no lighting (the "unlit" builtin shader -
    // same vertex path as forward, so it casts shadows and moves through the post stack normally).
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

} // namespace draconic::materials
