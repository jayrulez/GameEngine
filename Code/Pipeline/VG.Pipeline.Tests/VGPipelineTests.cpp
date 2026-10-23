// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Vector image pipeline: a linked .svg cooks when the engine reads it and fails when not, the
// product binds through VG.Resource's factory, the importer takes .svg, and another cook reads
// the image from its product or its source.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import pipeline.importer;
import foundation.vg.resource;
import vg.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::resource;
using namespace foundation::vg;
namespace content = foundation::content;

namespace
{
    constexpr StringView kHeartSvg =
        u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        u8"<path d=\"M12 21 L3 12 A5 5 0 0 1 12 5 A5 5 0 0 1 21 12 Z\" fill=\"#E53935\"/></svg>";

    void WriteText(StringView path, StringView text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }

    void RemoveAll(StringView dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(reinterpret_cast<const char*>(String(dir).CStr())), ec);
    }

    /// A Sources/ tree with the heart and a file that is no SVG, and an output database.
    struct Bed
    {
        static constexpr StringView kSources = u8"scratch_vgpipe_src";
        static constexpr StringView kDb = u8"scratch_vgpipe_db";

        Bed()
        {
            RegisterVGResource();
            RegisterVGAssets();
            RemoveAll(kSources);
            RemoveAll(kDb);
            REQUIRE(CreateDirectories(kSources));
            REQUIRE(CreateDirectories(kDb));
            WriteText(PathJoin(kSources, u8"heart.svg").AsView(), kHeartSvg);
            WriteText(PathJoin(kSources, u8"broken.svg").AsView(), u8"<html>not a vector image</html>");
            sources = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), kSources, DefaultAllocator());
            mount = MakeUnique<foundation::vfs::NativeFileSystem>(DefaultAllocator(), kDb, DefaultAllocator());
            db = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *mount, BinarySerializerFactory(), u8".rasset");
        }
        ~Bed()
        {
            db.Reset();
            mount.Reset();
            sources.Reset();
            RemoveAll(kSources);
            RemoveAll(kDb);
        }

        Status Cook(StringView file, content::Instance* output)
        {
            VectorImageAsset asset;
            asset.fileName = foundation::vfs::SourcePath(file);
            AssetBuildContext ctx{DefaultAllocator()};
            ctx.sources = sources.Get();
            ctx.output = output;
            return VectorImageAssetBuilder{}.Build(asset, ctx);
        }

        UniquePtr<foundation::vfs::NativeFileSystem> sources;
        UniquePtr<foundation::vfs::NativeFileSystem> mount;
        UniquePtr<content::ContentDatabase> db;
    };
}

TEST_CASE("vg.pipeline: a vector image cooks when it is an SVG the engine reads, and fails when not")
{
    Bed bed;
    content::Instance* heart = bed.db->RootGroup()->CreateInstance(u8"heart", VectorImageSource::StaticType());
    REQUIRE(bed.Cook(u8"heart.svg", heart).IsOk());
    CHECK_FALSE(bed.Cook(u8"broken.svg", bed.db->RootGroup()->CreateInstance(u8"broken", VectorImageSource::StaticType())).IsOk());
    CHECK_FALSE(bed.Cook(u8"", bed.db->RootGroup()->CreateInstance(u8"unlinked", VectorImageSource::StaticType())).IsOk());

    VectorImageFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), *bed.db);
    manager.AddFactory(&factory);
    Proxy<VectorImage> image = manager.Bind<VectorImage>(heart->Id());
    REQUIRE(image);
    CHECK(image->svg.AsView() == kHeartSvg);
}

TEST_CASE("vg.pipeline: the importer takes an .svg, and only that")
{
    VectorImageFileImporter importer;
    CHECK(importer.Accepts(u8"svg"));
    CHECK_FALSE(importer.Accepts(u8"sss"));
    CHECK_FALSE(importer.Accepts(u8"png"));
}

TEST_CASE("vg.pipeline: another cook reads a vector image from its product, or from its source before it is cooked")
{
    Bed bed;
    // Cooked: the product's text.
    content::Instance* cooked = bed.db->RootGroup()->CreateInstance(u8"heart", VectorImageSource::StaticType());
    REQUIRE(bed.Cook(u8"heart.svg", cooked).IsOk());
    AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = bed.sources.Get();
    ctx.db = bed.db.Get();
    String svg;
    REQUIRE(ReadVectorImage(ctx, cooked->Id(), svg).IsOk());
    CHECK(svg.AsView() == kHeartSvg);

    // Not cooked yet: the asset in the source database, read through its linked file.
    content::Instance* source = bed.db->RootGroup()->CreateInstance(u8"heart-source", VectorImageAsset::StaticType());
    VectorImageAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"heart.svg");
    REQUIRE(source->WriteObject(asset).IsOk());
    svg = String{};
    REQUIRE(ReadVectorImage(ctx, source->Id(), svg).IsOk());
    CHECK(svg.AsView() == kHeartSvg);

    Guid unknown;
    REQUIRE(Guid::TryParse(u8"0b7e2f6a-1c3d-4e5f-8a9b-0c1d2e3f4a5b", unknown));
    CHECK(ReadVectorImage(ctx, unknown, svg).Code() == ErrorCode::NotFound);
}
