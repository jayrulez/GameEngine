// Draconic::MeshEditor - the `draconic.geometry.editor` module (tooling).
//
// Source-side mesh authoring + cook:
//   * StaticMeshAsset / SkinnedMeshAsset (editor::Asset): wrap a cooked
//     Static/SkinnedMeshSource. (A real pipeline cooks these from an imported model
//     via a ModelMesh->StaticMesh converter - the tooling we did not port; the asset
//     here carries the already-resolved source.)
//   * The asset builders write the resolved source into the product DB (where the
//     mesh factories build it into a Static/SkinnedMesh).
//   * MeshImporter: capture a Static/SkinnedMesh built in code (e.g. a primitive)
//     into an asset.
//
// Never linked by the runtime.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.geometry.editor;

import draconic.foundation;
import draconic.editor;
import draconic.content;
import draconic.geometry;
import draconic.geometry.resource;

using namespace draconic::foundation;

export namespace draconic::geometry
{

    class StaticMeshAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(StaticMeshAsset, draconic::editor::Asset)
    public:
        StaticMeshSource source;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar); // fileName (source model note)
            source.Serialize(ar);
        }
    };

    class SkinnedMeshAsset final : public draconic::editor::Asset
    {
        DRACONIC_OBJECT(SkinnedMeshAsset, draconic::editor::Asset)
    public:
        SkinnedMeshSource source;

        void Serialize(ISerializer& ar) override
        {
            draconic::editor::Asset::Serialize(ar);
            source.Serialize(ar);
        }
    };

    // Cooks a StaticMeshAsset -> StaticMeshSource in the output DB.
    class StaticMeshAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &StaticMeshAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &StaticMeshSource::StaticType();
        }
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const StaticMeshAsset& ma = static_cast<const StaticMeshAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            return ctx.output->WriteObject(
                const_cast<StaticMeshSource&>(ma.source)); // write pass doesn't mutate
        }
    };

    // Cooks a SkinnedMeshAsset -> SkinnedMeshSource in the output DB.
    class SkinnedMeshAssetBuilder final : public draconic::editor::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &SkinnedMeshAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SkinnedMeshSource::StaticType();
        }
        [[nodiscard]] Status Build(const draconic::editor::Asset& asset,
                                   draconic::editor::AssetBuildContext& ctx) override
        {
            const SkinnedMeshAsset& ma = static_cast<const SkinnedMeshAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            return ctx.output->WriteObject(const_cast<SkinnedMeshSource&>(ma.source));
        }
    };

    // Authoring helpers: capture a mesh built in code into an asset.
    class MeshImporter
    {
    public:
        static void Import(const StaticMesh& mesh, StaticMeshAsset& outAsset)
        {
            StaticMeshSource::FromMesh(mesh, outAsset.source);
        }
        static void Import(const SkinnedMesh& mesh, SkinnedMeshAsset& outAsset)
        {
            SkinnedMeshSource::FromMesh(mesh, outAsset.source);
        }
    };

    inline void RegisterMeshAssets()
    {
        GlobalTypeRegistry().Register(StaticMeshAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<StaticMeshAsset>();
        GlobalTypeRegistry().Register(SkinnedMeshAsset::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<SkinnedMeshAsset>();
    }

    DRACONIC_DEFINE_OBJECT_VERSIONED(StaticMeshAsset, "draconic::geometry",
                                     2) // v2 = Float4 tangent vertex blobs
    DRACONIC_DEFINE_OBJECT_VERSIONED(SkinnedMeshAsset, "draconic::geometry",
                                     2) // v2 = Float4 tangent vertex blobs

} // namespace draconic::geometry
