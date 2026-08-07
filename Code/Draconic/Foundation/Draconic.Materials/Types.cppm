/// Draconic::Materials - the `:types` partition.
///
/// Value-type vocabulary for the data-driven material model: property kinds (the
/// declared shape a material exposes to shaders), pipeline-state presets (blend /
/// depth / cull, orthogonal to shader variant flags), and the predefined vertex
/// layouts. A material is *data*: a list of MaterialPropertyDefs + a PipelineConfig
/// + a shader name - the MaterialSystem infers the GPU bind-group layout from the
/// declared properties, so a new material/shader needs no renderer changes.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.materials:types;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::materials
{

    // The kind of a declared material property. Scalars/vectors/matrices pack into a
    // uniform buffer; textures and samplers become bind-group entries.
    enum class MaterialPropertyType : u8
    {
        Float,
        Float2,
        Float3,
        Float4,
        Int,
        Int2,
        Int3,
        Int4,
        Matrix4x4,
        Texture2D,
        TextureCube,
        Sampler,
    };

    // One declared property: its name, kind, and (for uniforms) its byte offset/size
    // in the material uniform buffer. `name` is a view into the owning Material's
    // stable name backing (see Material::AddProperty).
    struct MaterialPropertyDef
    {
        StringView name;
        MaterialPropertyType type = MaterialPropertyType::Float;
        u32 binding = 0; // ordinal in the material's binding space
        u32 offset = 0;  // byte offset in the uniform buffer (uniforms)
        u32 size = 0;    // byte size (uniforms)

        [[nodiscard]] bool IsTexture() const noexcept
        {
            return type == MaterialPropertyType::Texture2D ||
                   type == MaterialPropertyType::TextureCube;
        }
        [[nodiscard]] bool IsSampler() const noexcept
        {
            return type == MaterialPropertyType::Sampler;
        }
        [[nodiscard]] bool IsUniform() const noexcept { return !IsTexture() && !IsSampler(); }

        // Packed size of a uniform property kind (0 for textures/samplers).
        [[nodiscard]] static u32 SizeOf(MaterialPropertyType t) noexcept
        {
            switch (t)
            {
            case MaterialPropertyType::Float:
            case MaterialPropertyType::Int:
                return 4;
            case MaterialPropertyType::Float2:
            case MaterialPropertyType::Int2:
                return 8;
            case MaterialPropertyType::Float3:
            case MaterialPropertyType::Int3:
                return 12;
            case MaterialPropertyType::Float4:
            case MaterialPropertyType::Int4:
                return 16;
            case MaterialPropertyType::Matrix4x4:
                return 64;
            default:
                return 0; // textures / samplers carry no uniform data
            }
        }
    };

    // Blend preset - resolved to concrete BlendState by the PSO builder.
    enum class BlendMode : u8
    {
        Opaque,
        Masked,
        AlphaBlend,
        Additive,
        Multiply,
        PremultipliedAlpha,
    };

    // Depth test/write preset.
    enum class DepthMode : u8
    {
        Disabled,
        ReadWrite,
        ReadOnly,
        WriteOnly,
    };

    // Face-culling preset (distinct from rhi::CullMode so the material layer stays
    // RHI-agnostic at the data level; mapped at PSO build time).
    enum class CullModeConfig : u8
    {
        None,
        Back,
        Front,
    };

    // Predefined vertex layouts - the byte formats meshes/sprites/etc. supply.
    enum class VertexLayoutType : u8
    {
        None,            // procedural (no vertex input)
        PositionOnly,    // float3                              - skybox / shadow depth
        PositionUVColor, // float3 + float2 + float4            - sprites / particles
        MeshNoTangent,   // float3 + float3 + float2 (32 bytes)
        Mesh,            // + color(ubyte4) + tangent(float3) (48 bytes)
        SkinnedMesh,     // + joints(uint2) + weights(float4) (72 bytes)
        Custom,
    };

    /// Reflects the render-state enums above (BlendMode/DepthMode/CullModeConfig/VertexLayoutType)
    /// so a reflected MaterialSource property of one of these types renders as a name dropdown.
    /// Idempotent; defined in TypesReflectionImpl.cpp. Call from a startup registrar.
    void RegisterMaterialsTypeReflection();

} // namespace draconic::materials
