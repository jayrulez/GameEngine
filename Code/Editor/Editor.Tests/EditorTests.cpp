// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Basic checks for the asset-pipeline base: the Asset serialize round-trip and the shape of
// builder dispatch.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
import foundation.core;
import foundation.content;
import pipeline.core;
import foundation.vfs;
using namespace foundation::core;
    using namespace pipeline;

namespace
{
    // A concrete asset: a source file and one setting.
    class WidgetAsset final : public Asset
    {
        RTTI_OBJECT(WidgetAsset, Asset)
    public:
        i32 quality = 0;
        void Serialize(ISerializer& ar) override
        {
            Asset::Serialize(ar); // fileName
            foundation::core::Serialize(ar, "quality", quality);
        }
    };
}
RTTI_DEFINE_OBJECT(WidgetAsset, "rtti::editor::editor::test")

TEST_CASE("editor: Asset carries a source file path + settings (round-trips)")
{
    WidgetAsset a;
    a.fileName = foundation::vfs::SourcePath(u8"art/widget.png");
    a.quality = 7;

    MemoryStream buffer;
    {
        BinarySerializer w(buffer, SerializeMode::Write);
        a.Serialize(w);
        REQUIRE(w.IsOk());
    }
    REQUIRE(buffer.Seek(0, SeekOrigin::Begin) == 0);

    WidgetAsset b;
    {
        BinarySerializer r(buffer, SerializeMode::Read);
        b.Serialize(r);
        REQUIRE(r.IsOk());
    }
    CHECK(b.fileName == StringView(u8"art/widget.png"));
    CHECK(b.quality == 7);
}

namespace
{
    class WidgetBuilder final : public DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &WidgetAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &WidgetAsset::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return 3; }
        void ScanDependencies(const Asset&, AssetBuildContext&, AssetDependencies& out) override
        {
            out.files.PushBack(foundation::vfs::SourcePath(u8"extra.bin"));
        }
        [[nodiscard]] Status Build(const Asset&, AssetBuildContext&) override { return Status{}; }
    };
}

TEST_CASE("editor: source files read through the VFS mount")
{
    foundation::vfs::NativeFileSystem mount(u8".", foundation::core::DefaultAllocator());
    const byte payload[3] = {byte{'a'}, byte{'b'}, byte{'c'}};
    REQUIRE(mount.AsWritable()->Save(u8"editor_vfs_src.txt", Span<const byte>(payload, 3)).IsOk());

    AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &mount;

    Result<Array<byte>> bytes = DefaultAssetBuilder::ReadSourceBytes(ctx, u8"editor_vfs_src.txt");
    REQUIRE(bytes.HasValue());
    CHECK(bytes.Value().Size() == 3u);

    String text;
    REQUIRE(DefaultAssetBuilder::ReadSourceText(ctx, u8"editor_vfs_src.txt", text).IsOk());
    CHECK(text == StringView(u8"abc"));

    // A missing file or a missing mount fails cleanly.
    CHECK_FALSE(DefaultAssetBuilder::ReadSourceBytes(ctx, u8"editor_vfs_missing.txt").HasValue());
    AssetBuildContext empty{DefaultAllocator()};
    CHECK_FALSE(DefaultAssetBuilder::ReadSourceBytes(empty, u8"editor_vfs_src.txt").HasValue());

    REQUIRE(mount.AsWritable()->Delete(u8"editor_vfs_src.txt").IsOk());
}

TEST_CASE("editor: builder registry routes by asset type; v2 hooks surface")
{
    BuilderRegistry registry{DefaultAllocator()};
    CHECK(registry.Find(&WidgetAsset::StaticType()) == nullptr);

    registry.Register(
        UniquePtr<IAssetBuilder>(DefaultAllocator().New<WidgetBuilder>(), DefaultAllocator()));
    CHECK(registry.Count() == 1u);

    IAssetBuilder* builder = registry.Find(&WidgetAsset::StaticType());
    REQUIRE(builder != nullptr);
    CHECK(builder->Version() == 3u);
    CHECK(registry.FindByTypeName(u8"WidgetAsset") == builder);
    CHECK(registry.Find(&TypeOf<f32>()) == nullptr);

    // ScanDependencies collects declared extras; the default declares nothing.
    WidgetAsset asset;
    AssetBuildContext ctx{DefaultAllocator()};
    AssetDependencies deps;
    builder->ScanDependencies(asset, ctx, deps);
    REQUIRE(deps.files.Size() == 1u);
    CHECK(deps.files[0] == StringView(u8"extra.bin"));
    CHECK(deps.reads.IsEmpty());
    CHECK(deps.references.IsEmpty());
}
