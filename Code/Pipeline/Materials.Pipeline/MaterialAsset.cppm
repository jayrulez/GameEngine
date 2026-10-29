// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Materials - the `foundation.materials.editor` module (tooling).
//
// Source-side material authoring + cook:
//   * MaterialAsset (pipeline::Asset): wraps a MaterialSource (the authored material -
//     shader reference + declared properties + render-state presets + default
//     uniforms). A material has no external source file to decode, so the asset IS
//     the authored data; cooking resolves it into the product DB.
//   * MaterialAssetBuilder (DefaultAssetBuilder): writes the resolved MaterialSource
//     into the output DB (where MaterialFactory later builds it into a Material).
//   * MaterialImporter: fills a MaterialAsset from a Material authored in code + the
//     shader resource id it references.
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include "Core/Log/Log.h"

export module materials.pipeline;

import foundation.core;
import pipeline.core;
import foundation.content;
import foundation.materials;
import foundation.materials.resource;
import foundation.shaders;          // ShaderProperty: a custom shader's property table
import foundation.shaders.resource; // ShaderSource: the cooked custom shader a material reads

using namespace foundation::core;
using namespace foundation::materials;

export namespace pipeline{

    // Source asset wrapping the authored material data.
    class MaterialAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(MaterialAsset, pipeline::Asset)
    public:
        MaterialSource source;

        void Serialize(ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar); // fileName (optional authoring note)
            source.Serialize(ar);
        }
    };

    namespace detail
    {
        [[nodiscard]] inline MaterialPropertyType MaterialTypeOf(
            foundation::shaders::ShaderPropertyType type) noexcept
        {
            return foundation::materials::MaterialPropertyTypeOf(type);
        }

        [[nodiscard]] inline isize IndexOf(const Array<String>& names, StringView name) noexcept
        {
            for (usize i = 0; i < names.Size(); ++i)
            {
                if (names[i].AsView() == name)
                {
                    return static_cast<isize>(i);
                }
            }
            return -1;
        }

        // The shader's default for a uniform, written as the shader reads it (an int as an int).
        inline void WriteDefault(const foundation::shaders::ShaderProperty& p, u8* at) noexcept
        {
            if (p.type == foundation::shaders::ShaderPropertyType::Int)
            {
                const i32 value = static_cast<i32>(p.value.x);
                MemCopy(at, &value, sizeof(value));
                return;
            }
            const f32 values[4] = {p.value.x, p.value.y, p.value.z, p.value.w};
            MemCopy(at, values, p.size);
        }
    }

    // Carries a material's values onto another layout of it (`to`: its properties, offsets and
    // defaults already in place, no texture bound): each uniform whose name, type and size match
    // keeps its value, each texture binding whose slot `to` still has as a texture keeps its
    // texture, and the rest of `to` keeps its defaults. What does not carry is dropped with a
    // warning. A material re-laid by its shader's table (the cook) and switched to another shader
    // (the material page) keeps its values so.
    inline void CarryMaterialValues(const MaterialSource& from, MaterialSource& to)
    {
        const auto isTexture = [](u8 type)
        {
            const auto t = static_cast<MaterialPropertyType>(type);
            return t == MaterialPropertyType::Texture2D || t == MaterialPropertyType::TextureCube;
        };
        to.textureSlots.Clear();
        to.textureIds.Clear();
        for (usize i = 0; i < to.propertyNames.Size(); ++i)
        {
            const StringView name = to.propertyNames[i].AsView();
            const u8 type = i < to.propertyTypes.Size() ? to.propertyTypes[i] : 0u;
            const u32 size = i < to.propertySizes.Size() ? to.propertySizes[i] : 0u;
            if (size == 0u)
            {
                const isize slot = detail::IndexOf(from.textureSlots, name);
                if (isTexture(type) && slot >= 0 &&
                    static_cast<usize>(slot) < from.textureIds.Size())
                {
                    to.textureSlots.PushBack(String(name));
                    to.textureIds.PushBack(from.textureIds[static_cast<usize>(slot)]);
                }
                continue;
            }
            const isize index = detail::IndexOf(from.propertyNames, name);
            if (index < 0)
            {
                continue;
            }
            const usize k = static_cast<usize>(index);
            const bool sameKind = k < from.propertyTypes.Size() && k < from.propertyOffsets.Size() &&
                                  k < from.propertySizes.Size() && from.propertyTypes[k] == type &&
                                  from.propertySizes[k] == size;
            const u32 at = i < to.propertyOffsets.Size() ? to.propertyOffsets[i] : 0u;
            if (sameKind && from.propertyOffsets[k] + size <= from.uniformDefaults.Size() &&
                at + size <= to.uniformDefaults.Size())
            {
                MemCopy(to.uniformDefaults.Data() + at,
                        from.uniformDefaults.Data() + from.propertyOffsets[k], size);
            }
        }

        for (usize i = 0; i < from.propertyNames.Size(); ++i)
        {
            if (detail::IndexOf(to.propertyNames, from.propertyNames[i].AsView()) < 0)
            {
                LOG_WARNING(u8"Materials",
                            u8"material '{}': '{}' is not a property of shader '{}' - dropped",
                            from.name, from.propertyNames[i], to.shaderName);
            }
        }
        for (usize i = 0; i < from.textureSlots.Size(); ++i)
        {
            if (detail::IndexOf(to.textureSlots, from.textureSlots[i].AsView()) < 0)
            {
                LOG_WARNING(u8"Materials",
                            u8"material '{}': shader '{}' has no texture '{}' - its binding "
                            u8"is dropped",
                            from.name, to.shaderName, from.textureSlots[i]);
            }
        }
    }

    // Keeps the effect groups of `from` whose effect `declared` (the new shader's vertex variants)
    // still declares, values and all, as `to`'s; drops the rest with a warning. A material moved
    // between two shaders that both declare WIND keeps its wind; one moved to a shader that does
    // not, loses it (Specs/wind-for-every-shader.md). A shader that declares an effect the
    // material has no group for gets none here: the factory adds it at the effect's defaults when
    // it has the shader system.
    inline void KeepDeclaredEffects(const MaterialSource& from, MaterialSource& to,
                                    foundation::shaders::ShaderFlags declared)
    {
        to.effects.Clear();
        for (const MaterialEffectGroupSource& group : from.effects)
        {
            const auto flag = static_cast<foundation::shaders::ShaderFlags>(group.flag);
            if (foundation::shaders::HasFlag(declared, flag))
            {
                to.effects.PushBack(group);
            }
            else
            {
                LOG_WARNING(u8"Materials",
                            u8"material '{}': shader '{}' does not declare the {} effect - its "
                            u8"group is dropped",
                            from.name, to.shaderName, group.name);
            }
        }
    }

    // A material on a custom shader, laid out by the shader's property table: each property in
    // the shader's order and at its offset, at the shader's default, then the authored material's
    // values carried over (CarryMaterialValues). The cook lays a material out so, and the editor
    // shows and previews one so.
    inline void LayOutMaterialByShader(const MaterialSource& authored,
                                       const foundation::shaders::ShaderSource& shader,
                                       MaterialSource& out)
    {
        namespace sh = foundation::shaders;
        // Name, shader reference, render state and samplers come from the authored material.
        out.name = authored.name;
        out.shaderId = authored.shaderId;
        out.shaderName = shader.name;
        out.shaderFlags = authored.shaderFlags;
        out.blendMode = authored.blendMode;
        out.depthMode = authored.depthMode;
        out.cullMode = authored.cullMode;
        out.vertexLayout = authored.vertexLayout;
        out.samplerU = authored.samplerU;
        out.samplerV = authored.samplerV;
        out.propertyNames.Clear();
        out.propertyTypes.Clear();
        out.propertyBindings.Clear();
        out.propertyOffsets.Clear();
        out.propertySizes.Clear();
        out.uniformDefaults.Clear();
        out.uniformDefaults.Resize(shader.uniformSize);
        for (u8& b : out.uniformDefaults)
        {
            b = 0;
        }
        const Array<sh::ShaderProperty> table = shader.Properties();
        for (usize i = 0; i < table.Size(); ++i)
        {
            const sh::ShaderProperty& p = table[i];
            out.propertyNames.PushBack(p.name);
            out.propertyTypes.PushBack(static_cast<u8>(detail::MaterialTypeOf(p.type)));
            out.propertyBindings.PushBack(static_cast<u32>(i));
            out.propertyOffsets.PushBack(p.offset);
            out.propertySizes.PushBack(p.size);
            if (p.size != 0u)
            {
                detail::WriteDefault(p, out.uniformDefaults.Data() + p.offset);
            }
        }
        CarryMaterialValues(authored, out);
        KeepDeclaredEffects(authored, out, static_cast<foundation::shaders::ShaderFlags>(shader.vertexVariantMask));
    }

    // Cooks a MaterialAsset -> MaterialSource in the output DB (the authored source is
    // already the runtime source; writing it is the cook).
    class MaterialAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        // Cook version 2 (2026-09-08): MaterialSource 3 - the property arrays use full names; a product cooked under the older record
        // version is refused by the reader, so the recipe hash must change to re-cook it.
        // Version 3: a material on a custom shader is laid out by the shader's property table.
        // Version 4: MaterialSource 4 - the effect groups, and PBR's wind lanes migrated into one.
        [[nodiscard]] u32 Version() const override { return 4; }

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &MaterialAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &MaterialSource::StaticType();
        }

        // Bound textures are runtime REFERENCES: their products must exist, but a texture edit
        // never re-cooks the material (the factory re-binds at load/reload).
        void ScanDependencies(const pipeline::Asset& asset,
                              pipeline::AssetBuildContext&,
                              pipeline::AssetDependencies& out) override
        {
            const MaterialAsset& ma = static_cast<const MaterialAsset&>(asset);
            for (const Guid& id : ma.source.textureIds)
            {
                if (!id.IsNil())
                {
                    out.references.PushBack(id);
                }
            }
            // A custom shader's property table lays the product out: its CONTENT is read, so a
            // shader edit (a property added, removed or retyped) re-cooks the material.
            if (!ma.source.shaderId.IsNil())
            {
                out.reads.PushBack(ma.source.shaderId);
            }
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const MaterialAsset& ma =
                static_cast<const MaterialAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (ma.source.shaderId.IsNil())
            {
                // A built-in shader: the authored source is the runtime source. WriteObject only
                // reads during a write pass; const_cast is safe here.
                return ctx.output->WriteObject(const_cast<MaterialSource&>(ma.source));
            }

            // A custom shader: its cooked property table is the layout, the asset the values.
            foundation::content::Instance* instance =
                ctx.db != nullptr ? ctx.db->GetInstance(ma.source.shaderId) : nullptr;
            RefPtr<ISerializable> object =
                instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
            const auto* shader = Cast<foundation::shaders::ShaderSource>(object.Get());
            if (shader == nullptr)
            {
                LOG_ERROR(u8"Materials", u8"material '{}': its shader has not cooked - cook failed",
                          ma.source.name);
                return Status{ErrorCode::NotFound};
            }
            MaterialSource product;
            LayOutMaterialByShader(ma.source, *shader, product);
            return ctx.output->WriteObject(product);
        }
    };

    // Authoring helper: capture a Material (built in code) + its shader id into an asset.
    class MaterialImporter
    {
    public:
        static void Import(const Material& material, const Guid& shaderId, MaterialAsset& outAsset)
        {
            MaterialSource::FromMaterial(material, shaderId, outAsset.source);
        }
    };

    // Replaces `to` with `from`, every field (MaterialSource is not copyable; its serialization is
    // the complete list of what it holds).
    inline void AssignMaterialSource(MaterialSource& to, MaterialSource& from)
    {
        MemoryStream stream;
        {
            BinarySerializer write(stream, SerializeMode::Write);
            from.Serialize(write);
        }
        (void)stream.Seek(0, SeekOrigin::Begin);
        BinarySerializer read(stream, SerializeMode::Read);
        to.Serialize(read);
    }

    // Puts a material on a custom shader (`shaderId`, whose cooked `shader` is given): laid out by
    // the shader's table, every value whose name, type and size still fit kept. What the material
    // page's Shader row and material_set_shader do, so a saved material is in its shader's layout
    // (a cook re-lays it anyway, but a material left in another layout warns on every cook).
    inline void SetMaterialShader(MaterialSource& source, const Guid& shaderId,
                                  const foundation::shaders::ShaderSource& shader)
    {
        source.shaderId = shaderId;
        MaterialSource laid;
        LayOutMaterialByShader(source, shader, laid);
        AssignMaterialSource(source, laid);
    }

    // Puts a material back on a built-in shader (PBR, Unlit or Phong): the built-in's own layout
    // (what Create > Materials makes), the material's values and render state carried onto it.
    inline void SetMaterialBuiltInShader(MaterialSource& source, foundation::materials::BuiltinShading shading)
    {
        RefPtr<Material> built = foundation::materials::CreateBuiltin(shading, source.name.AsView());
        MaterialSource to;
        MaterialSource::FromMaterial(*built, Guid{}, to);
        to.name = source.name;
        to.blendMode = source.blendMode;
        to.depthMode = source.depthMode;
        to.cullMode = source.cullMode;
        to.vertexLayout = source.vertexLayout;
        to.samplerU = source.samplerU;
        to.samplerV = source.samplerV;
        CarryMaterialValues(source, to);
        KeepDeclaredEffects(source, to, foundation::materials::BuiltinShadingOf(shading).vertexVariants);
        AssignMaterialSource(source, to);
    }

    // Registers MaterialAsset for content-DB construction + deserialization.
    inline void RegisterMaterialAsset()
    {
        RegisterMaterialsTypeReflection(); // the render-state enums the source's fields resolve to
        GlobalTypeRegistry().Register(MaterialAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<MaterialAsset>();
    }

    // MaterialAsset::StaticType() is defined WITH its reflected surface (a Nested `source`
    // property) in MaterialAssetImpl.cpp - GCC module hygiene: REFLECT_MEMBERS out of interfaces.


    /// A material on the built-in `shading` in `target`, named `baseName` made unique.
    [[nodiscard]] foundation::content::Instance* CreateMaterialInstance(
        foundation::content::Group* target, StringView baseName, foundation::materials::BuiltinShading shading);
    /// File > New's material creators (pipeline.registration composes every domain's).
    void RegisterMaterialCreators(AssetCreatorRegistry& registry);
} // namespace foundation::materials
