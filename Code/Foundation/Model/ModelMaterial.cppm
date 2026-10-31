// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// A model's material as its file authored it: metal-rough PBR, or the lighting model a legacy
/// format names (Phong, Lambert, unlit), which the import makes the matching built-in material of.

module;
#include "Core/Prelude.h"

#include <string>

export module foundation.model:model_material;

import foundation.core;

using namespace foundation::core;

export namespace foundation::model
{

    /// Alpha blending mode.
    enum class AlphaMode : u32
    {
        Opaque,
        Mask,
        Blend,
    };

    /// How a material was authored to be lit: the built-in material the import makes of it.
    enum class ShadingModel : u8
    {
        Pbr,     ///< metal-rough PBR (glTF's, and an FBX or OBJ material with PBR data)
        Phong,   ///< diffuse plus a specular highlight (an FBX Phong material, a classic OBJ .mtl)
        Lambert, ///< diffuse alone (an FBX Lambert material)
        Unlit,   ///< its colour as it is, no light (glTF KHR_materials_unlit)
    };

    /// A model's material: its shading model and what that model reads.
    class ModelMaterial
    {
    public:
        ModelMaterial() = default;
        ~ModelMaterial() = default;

        [[nodiscard]] StringView name() const { return StringView(m_name.Data(), m_name.Size()); }
        void setName(StringView n) { m_name = String(n); }

        // How it is lit; PBR unless its loader says otherwise.
        ShadingModel shadingModel = ShadingModel::Pbr;

        // -- Base color (Phong's and Lambert's diffuse, Unlit's colour) --
        // Colour factors are authored sRGB, as every colour is (the material property they become
        // decodes them for the GPU). A loader whose format stores them linear (glTF, FBX) encodes them.
        Float4 baseColorFactor{1, 1, 1, 1};
        i32 baseColorTextureIndex = -1;

        // -- Metallic-Roughness --
        f32 metallicFactor = 1.0f;
        f32 roughnessFactor = 1.0f;
        // glTF-style packed texture (G = roughness, B = metallic).
        i32 metallicRoughnessTextureIndex = -1;
        // FBX-style separate grayscale maps. The importer bakes them into a packed texture, since
        // feeding either one directly into the packed slot bleeds across channels.
        i32 separateRoughnessTextureIndex = -1;
        i32 separateMetalnessTextureIndex = -1;

        // -- Normal map --
        f32 normalScale = 1.0f;
        i32 normalTextureIndex = -1;

        // -- Occlusion --
        f32 occlusionStrength = 1.0f;
        i32 occlusionTextureIndex = -1;

        // -- Emissive --
        Float3 emissiveFactor{};       // sRGB, like baseColorFactor
        f32 emissiveIntensity = 1.0f; // linear multiplier (glTF KHR_materials_emissive_strength)
        i32 emissiveTextureIndex = -1;

        // -- The highlight (Phong) --
        Float3 specularColor{};        // sRGB, times the file's specular factor; black: none
        i32 specularTextureIndex = -1;
        f32 shininess = 20.0f;         // the highlight's exponent

        // -- Alpha --
        AlphaMode alphaMode = AlphaMode::Opaque;
        f32 alphaCutoff = 0.5f;

        // -- Double-sided --
        bool doubleSided = false;

    private:
        String m_name;
    };

} // namespace foundation::model
