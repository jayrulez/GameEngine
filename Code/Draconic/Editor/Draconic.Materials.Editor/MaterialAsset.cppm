// Draconic::MaterialEditor - the `draconic.materials.editor` module (tooling).
//
// Source-side material authoring + cook:
//   * MaterialAsset (editor::Asset): wraps a MaterialSource (the authored material -
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
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.materials.editor;

import draconic.foundation;
import draconic.editor;
import draconic.content;
import draconic.materials;
import draconic.materials.resource;

using namespace draconic::foundation;

export namespace draconic::materials
{

    // Source asset wrapping the authored material data.
    class MaterialAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(MaterialAsset, draconic::editor::Asset)
    public:
        MaterialSource source;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName (optional authoring note)
            source.Serialize(ar);
        }
    };

    // Cooks a MaterialAsset -> MaterialSource in the output DB (the authored source is
    // already the runtime source; writing it is the cook).
    class MaterialAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
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
        void ScanDependencies(const draconic::editor::Asset& asset,
                              draconic::editor::AssetBuildContext&,
                              draconic::editor::AssetDependencies& out) override
        {
            const MaterialAsset& ma = static_cast<const MaterialAsset&>(asset);
            for (const Guid& id : ma.source.textureIds)
            {
                if (!id.IsNil())
                {
                    out.references.PushBack(id);
                }
            }
        }

        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const MaterialAsset& ma =
                static_cast<const MaterialAsset&>(asset); // guarded by AssetType()
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            // WriteObject only reads during a write pass; const_cast is safe here.
            return ctx.output->WriteObject(const_cast<MaterialSource&>(ma.source));
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

    // Registers MaterialAsset for content-DB construction + deserialization.
    inline void RegisterMaterialAsset()
    {
        RegisterMaterialsTypeReflection(); // the render-state enums the source's fields resolve to
        GlobalTypeRegistry().Register(MaterialAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<MaterialAsset>();
    }

    // MaterialAsset::StaticType() is defined WITH its reflected surface (a Nested `source`
    // property) in MaterialAssetImpl.cpp - GCC module hygiene: DRACONIC_REFLECT out of interfaces.

} // namespace draconic::materials
