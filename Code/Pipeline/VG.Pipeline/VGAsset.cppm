// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::VG - the `vg.pipeline` module (tooling).
//
// Vector images, source side:
//   * VectorImageAsset: a LINKED `.svg` in the project's Sources/ tree (Asset::fileName), like a
//     UI document or a script; an asset with no linked file fails the cook.
//   * VectorImageAssetBuilder VALIDATES at cook: the document must load as the renderer reads it
//     (VG.SVG's loader) or the cook FAILS; then the text goes through to the cooked record
//     (foundation.vg.resource's VectorImageSource).
//   * VectorImageFileImporter: a dropped `.svg` is staged into Sources/ and linked.
//   * ReadVectorImage: what another cook embeds (a UI theme's `@icon`).
//
// Never linked by the runtime.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

export module vg.pipeline;

import foundation.core;
import pipeline.core;
import pipeline.importer;
import foundation.content;
import foundation.vg.svg; // a vector image validates by loading
import foundation.vg.resource;

using namespace foundation::core;
using namespace foundation::vg;

export namespace pipeline
{
    /// A vector image: a LINKED `.svg` in Sources/.
    class VectorImageAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(VectorImageAsset, pipeline::Asset)
    public:
        void Serialize(ISerializer& ar) override { pipeline::Asset::Serialize(ar); }
    };

    class VectorImageAssetBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &VectorImageAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &VectorImageSource::StaticType();
        }

        [[nodiscard]] Status Build(const pipeline::Asset& asset,
                                   pipeline::AssetBuildContext& ctx) override
        {
            const VectorImageAsset& va = static_cast<const VectorImageAsset&>(asset);
            if (ctx.output == nullptr)
            {
                return Status{ErrorCode::InvalidArgument};
            }
            if (va.fileName.IsEmpty())
            {
                LOG_ERROR(u8"VG", u8"vector image has no linked source file - cook failed");
                return Status{ErrorCode::InvalidArgument};
            }
            String svg;
            if (const Status read = ReadSourceText(ctx, va.fileName.View(), svg); !read.IsOk())
            {
                LOG_ERROR(u8"VG", u8"vector image '{}': source file missing - cook failed",
                          va.fileName.View());
                return read;
            }
            // Validation is the cook: the document must load as the renderer reads it.
            if (!foundation::vg::svg::SVGLoader::Load(svg.AsView()).HasValue())
            {
                LOG_ERROR(u8"VG", u8"vector image '{}' is not an SVG this engine reads - cook failed",
                          va.fileName.View());
                return Status{ErrorCode::InvalidArgument};
            }
            VectorImageSource cooked;
            cooked.svg = Move(svg);
            return ctx.output->WriteObject(cooked);
        }
    };

    /// The SVG a vector image asset carries: its cooked product when the cook has made it, else
    /// its linked source file (a headless or source-database cook). NotFound when `id` names no
    /// vector image.
    inline Status ReadVectorImage(const pipeline::AssetBuildContext& ctx, const Guid& id, String& out)
    {
        foundation::content::IContentDatabase* dbs[] = {ctx.db, ctx.sourceDb};
        for (foundation::content::IContentDatabase* db : dbs)
        {
            foundation::content::Instance* instance = db != nullptr ? db->GetInstance(id) : nullptr;
            if (instance == nullptr)
            {
                continue;
            }
            RefPtr<ISerializable> object = instance->ReadObject();
            if (const VectorImageSource* cooked = Cast<VectorImageSource>(object.Get()))
            {
                out = String(cooked->svg.AsView(), *ctx.allocator);
                return Status{};
            }
            if (const VectorImageAsset* asset = Cast<VectorImageAsset>(object.Get()))
            {
                return pipeline::DefaultAssetBuilder::ReadSourceText(ctx, asset->fileName.View(), out);
            }
        }
        return Status{ErrorCode::NotFound};
    }

    /// Drag-drop importer for `.svg` files: the file is STAGED into the project's Sources/ tree
    /// and the asset LINKS it through fileName; the cook reads it back through the sources mount.
    class VectorImageFileImporter final : public pipeline::IFileImporter
    {
    public:
        [[nodiscard]] StringView Label() const override { return u8"Vector image"; }
        [[nodiscard]] bool Accepts(StringView extension) const override { return extension == u8"svg"; }

        [[nodiscard]] pipeline::ImportPlan DescribeImport(StringView sourcePath,
                                                          const pipeline::ImportOptions*,
                                                          Object*) override
        {
            return pipeline::SingleAssetPlan(sourcePath); // one asset, named after the stem
        }

        [[nodiscard]] StringView AssetTypeName(StringView) const override { return u8"VectorImageAsset"; }

        [[nodiscard]] pipeline::ImportPlan StoredSelection(foundation::content::Group& group,
                                                           StringView sourcePath) override
        {
            return pipeline::SingleAssetStoredSelection(group, sourcePath, u8"VectorImageAsset");
        }

        [[nodiscard]] Result<foundation::content::Instance*>
        Import(StringView sourcePath, const pipeline::ImportContext& context,
               foundation::content::Group& group,
               const pipeline::ImportOptions* options, Object*,
               Array<pipeline::DeferredImportWrite>*) override
        {
            // The name claimed by the clash rule, the file copied in beside any other's (pipeline::ClaimSingleAsset).
            String fileName;
            Result<foundation::content::Instance*> claimed = pipeline::ClaimSingleAsset(context, group, options, sourcePath, VectorImageAsset::StaticType(), fileName);
            if (!claimed.HasValue())
            {
                return Err(claimed.Error());
            }
            foundation::content::Instance* instance = claimed.Value();
            VectorImageAsset asset;
            asset.fileName = foundation::vfs::SourcePath(fileName.AsView());
            if (const Status written = instance->WriteObject(asset); !written.IsOk())
            {
                return Err(written.Code());
            }
            return instance;
        }
    };

    inline void RegisterVGAssets()
    {
        GlobalTypeRegistry().Register(VectorImageAsset::StaticType(), TypeDomain(u8"Pipeline"));
        RegisterSerializable<VectorImageAsset>();
    }

    // VectorImageAsset's StaticType() is defined WITH its reflected attributes in VGAssetImpl.cpp.
}
