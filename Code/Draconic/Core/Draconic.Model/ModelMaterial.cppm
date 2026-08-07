/// Material properties for PBR rendering.
/// Ported from Sedulous.Models/ModelMaterial.bf.

module;
#include "Draconic.Foundation/Prelude.h"

#include <string>

export module draconic.model:model_material;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::model
{

    /// Alpha blending mode.
    enum class AlphaMode : u32
    {
        Opaque,
        Mask,
        Blend,
    };

    /// PBR material properties for a model.
    class ModelMaterial
    {
    public:
        ModelMaterial() = default;
        ~ModelMaterial() = default;

        [[nodiscard]] StringView name() const { return StringView(m_name.Data(), m_name.Size()); }
        void setName(StringView n) { m_name = String(n); }

        // -- Base color --
        Float4 baseColorFactor{1, 1, 1, 1};
        i32 baseColorTextureIndex = -1;

        // -- Metallic-Roughness --
        f32 metallicFactor = 1.0f;
        f32 roughnessFactor = 1.0f;
        // glTF-style PACKED texture (G = roughness, B = metallic).
        i32 metallicRoughnessTextureIndex = -1;
        // FBX-style SEPARATE grayscale maps (the importer bakes them into a packed texture -
        // feeding either one directly into the packed slot bleeds across channels).
        i32 separateRoughnessTextureIndex = -1;
        i32 separateMetalnessTextureIndex = -1;

        // -- Normal map --
        f32 normalScale = 1.0f;
        i32 normalTextureIndex = -1;

        // -- Occlusion --
        f32 occlusionStrength = 1.0f;
        i32 occlusionTextureIndex = -1;

        // -- Emissive --
        Float3 emissiveFactor{};
        i32 emissiveTextureIndex = -1;

        // -- Alpha --
        AlphaMode alphaMode = AlphaMode::Opaque;
        f32 alphaCutoff = 0.5f;

        // -- Double-sided --
        bool doubleSided = false;

    private:
        String m_name;
    };

} // namespace draconic::model
