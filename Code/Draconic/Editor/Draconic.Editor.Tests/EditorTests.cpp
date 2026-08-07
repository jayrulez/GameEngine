// Basic checks for the asset-pipeline base (Asset serialize round-trip + builder dispatch shape).
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
import draconic.foundation;
import draconic.content;
import draconic.editor;
import draconic.vfs;
using namespace draconic::foundation;
using namespace draconic::editor;

namespace
{
    // A concrete asset: source file + one setting.
    class WidgetAsset final : public Asset
    {
        DRACONIC_OBJECT(WidgetAsset, Asset)
    public:
        i32 quality = 0;
        void Serialize(ISerializer& ar) override
        {
            Asset::Serialize(ar); // fileName
            draconic::foundation::Serialize(ar, "quality", quality);
        }
    };
}
DRACONIC_DEFINE_OBJECT(WidgetAsset, "draconic::editor::test")

TEST_CASE("editor: Asset carries a source file path + settings (round-trips)")
{
    WidgetAsset a;
    a.fileName = draconic::vfs::SourcePath(u8"art/widget.png");
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
            out.files.PushBack(draconic::vfs::SourcePath(u8"extra.bin"));
        }
        [[nodiscard]] Status Build(const Asset&, AssetBuildContext&) override { return Status{}; }
    };
}

TEST_CASE("editor: source files read through the VFS mount")
{
    draconic::vfs::NativeFileSystem mount(u8".");
    const byte payload[3] = {byte{'a'}, byte{'b'}, byte{'c'}};
    REQUIRE(mount.AsWritable()->Save(u8"editor_vfs_src.txt", Span<const byte>(payload, 3)).IsOk());

    AssetBuildContext ctx;
    ctx.sources = &mount;

    Result<Array<byte>> bytes = DefaultAssetBuilder::ReadSourceBytes(ctx, u8"editor_vfs_src.txt");
    REQUIRE(bytes.HasValue());
    CHECK(bytes.Value().Size() == 3u);

    String text;
    REQUIRE(DefaultAssetBuilder::ReadSourceText(ctx, u8"editor_vfs_src.txt", text).IsOk());
    CHECK(text == StringView(u8"abc"));

    // Missing file / missing mount are clean failures.
    CHECK_FALSE(DefaultAssetBuilder::ReadSourceBytes(ctx, u8"editor_vfs_missing.txt").HasValue());
    AssetBuildContext empty;
    CHECK_FALSE(DefaultAssetBuilder::ReadSourceBytes(empty, u8"editor_vfs_src.txt").HasValue());

    REQUIRE(mount.AsWritable()->Delete(u8"editor_vfs_src.txt").IsOk());
}

TEST_CASE("editor: builder registry routes by asset type; v2 hooks surface")
{
    BuilderRegistry registry;
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
    AssetBuildContext ctx;
    AssetDependencies deps;
    builder->ScanDependencies(asset, ctx, deps);
    REQUIRE(deps.files.Size() == 1u);
    CHECK(deps.files[0] == StringView(u8"extra.bin"));
    CHECK(deps.reads.IsEmpty());
    CHECK(deps.references.IsEmpty());
}
