// Draconic::Materials - reflection implementation unit: the material render-state enums.
//
// Reflected in their OWNING module (draconic.materials) so any consumer of a reflected
// MaterialSource property whose type is one of these (blendMode/depthMode/cullMode/vertexLayout,
// retyped from u8) sees a proper enum - IsEnum + named values, so tooling can render a name
// dropdown instead of a raw integer. DRACONIC_REFLECT_ENUM bodies live out of the interface
// (GCC module hygiene). RegisterMaterialsTypeReflection() is idempotent; wire it from a startup
// registrar (RegisterMaterialAsset does). Reflection track P1 (the enum-retype payoff).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

module draconic.materials;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::materials
{
    DRACONIC_REFLECT_ENUM(BlendMode, "draconic::materials")
    {
        builder.Value("Opaque", BlendMode::Opaque);
        builder.Value("Masked", BlendMode::Masked);
        builder.Value("AlphaBlend", BlendMode::AlphaBlend);
        builder.Value("Additive", BlendMode::Additive);
        builder.Value("Multiply", BlendMode::Multiply);
        builder.Value("PremultipliedAlpha", BlendMode::PremultipliedAlpha);
    }

    DRACONIC_REFLECT_ENUM(DepthMode, "draconic::materials")
    {
        builder.Value("Disabled", DepthMode::Disabled);
        builder.Value("ReadWrite", DepthMode::ReadWrite);
        builder.Value("ReadOnly", DepthMode::ReadOnly);
        builder.Value("WriteOnly", DepthMode::WriteOnly);
    }

    DRACONIC_REFLECT_ENUM(CullModeConfig, "draconic::materials")
    {
        builder.Value("None", CullModeConfig::None);
        builder.Value("Back", CullModeConfig::Back);
        builder.Value("Front", CullModeConfig::Front);
    }

    DRACONIC_REFLECT_ENUM(VertexLayoutType, "draconic::materials")
    {
        builder.Value("None", VertexLayoutType::None);
        builder.Value("PositionOnly", VertexLayoutType::PositionOnly);
        builder.Value("PositionUVColor", VertexLayoutType::PositionUVColor);
        builder.Value("MeshNoTangent", VertexLayoutType::MeshNoTangent);
        builder.Value("Mesh", VertexLayoutType::Mesh);
        builder.Value("SkinnedMesh", VertexLayoutType::SkinnedMesh);
        builder.Value("Custom", VertexLayoutType::Custom);
    }

    void RegisterMaterialsTypeReflection()
    {
        static const bool once = []()
        {
            DraconicRegisterEnum_BlendMode();
            DraconicRegisterEnum_DepthMode();
            DraconicRegisterEnum_CullModeConfig();
            DraconicRegisterEnum_VertexLayoutType();
            return true;
        }();
        (void)once;
    }
}
