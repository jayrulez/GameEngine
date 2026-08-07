// CookDriver headless tests: the recipe-hash staleness model end to end. Fixture = four
// NativeFileSystem mounts (source DB, cooked DB, sources, cache) under a temp tree, one
// file-reading builder + one dependency-declaring builder.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"
#include <initializer_list>

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.editor;
import draconic.editor.cook;

using namespace draconic::foundation;
using namespace draconic::editor;
namespace content = draconic::content;
namespace vfs = draconic::vfs;

namespace
{
    // === Test asset types ===

    class CookWidgetAsset final : public Asset
    {
        DRACONIC_OBJECT(CookWidgetAsset, Asset)
    public:
        i32 quality = 1;
        void Serialize(ISerializer& ar) override
        {
            Asset::Serialize(ar);
            draconic::foundation::Serialize(ar, "quality", quality);
        }
    };

    class CookWidgetProduct final : public ISerializable
    {
        DRACONIC_OBJECT(CookWidgetProduct, ISerializable)
    public:
        i32 cookedValue = 0;
        void Serialize(ISerializer& ar) override
        {
            draconic::foundation::Serialize(ar, "cookedValue", cookedValue);
        }
    };

    class CookWidgetBuilder final : public DefaultAssetBuilder
    {
    public:
        static inline u32 version = 1;
        static inline bool fail = false;

        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &CookWidgetAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &CookWidgetProduct::StaticType();
        }
        [[nodiscard]] u32 Version() const override { return version; }

        [[nodiscard]] Status Build(const Asset& asset, AssetBuildContext& ctx) override
        {
            if (fail)
            {
                return Status{ErrorCode::Unknown};
            }
            const CookWidgetAsset& wa = static_cast<const CookWidgetAsset&>(asset);
            CookWidgetProduct product;
            product.cookedValue = wa.quality;
            if (!wa.fileName.IsEmpty())
            {
                Result<Array<byte>> bytes = ReadSourceBytes(ctx, wa.fileName.View());
                if (!bytes.HasValue())
                {
                    return Status{bytes.Error()};
                }
                product.cookedValue += static_cast<i32>(bytes.Value().Size());
            }
            return ctx.output->WriteObject(product);
        }
    };

    // An asset that READS one instance and REFERENCES another (design §3 dependency kinds).
    class ChainAsset final : public Asset
    {
        DRACONIC_OBJECT(ChainAsset, Asset)
    public:
        Guid readDep;
        Guid refDep;
        void Serialize(ISerializer& ar) override
        {
            Asset::Serialize(ar);
            draconic::foundation::Serialize(ar, "readDep", readDep);
            draconic::foundation::Serialize(ar, "refDep", refDep);
        }
    };

    class ChainBuilder final : public DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &ChainAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &CookWidgetProduct::StaticType();
        }
        void ScanDependencies(const Asset& asset, AssetBuildContext&,
                              AssetDependencies& out) override
        {
            const ChainAsset& ca = static_cast<const ChainAsset&>(asset);
            if (ca.readDep != Guid{})
            {
                out.reads.PushBack(ca.readDep);
            }
            if (ca.refDep != Guid{})
            {
                out.references.PushBack(ca.refDep);
            }
        }
        [[nodiscard]] Status Build(const Asset&, AssetBuildContext& ctx) override
        {
            CookWidgetProduct product;
            product.cookedValue = 42;
            return ctx.output->WriteObject(product);
        }
    };

    void EnsureRegistered()
    {
        static bool done = false;
        if (done)
        {
            return;
        }
        done = true;
        GlobalTypeRegistry().Register(CookWidgetAsset::StaticType());
        GlobalTypeRegistry().Register(CookWidgetProduct::StaticType());
        GlobalTypeRegistry().Register(ChainAsset::StaticType());
        RegisterSerializable<CookWidgetAsset>();
        RegisterSerializable<CookWidgetProduct>();
        RegisterSerializable<ChainAsset>();
    }

    // === Fixture: a temp project tree with the four mounts ===

    struct Fixture
    {
        String root;
        UniquePtr<vfs::NativeFileSystem> contentFs, cookedFs, sourcesFs, cacheFs;
        UniquePtr<content::ContentDatabase> sourceDb, cookedDb;
        BuilderRegistry builders;

        explicit Fixture(StringView name)
        {
            EnsureRegistered();
            root = String(name);
            RemoveTree();
            for (StringView dir : {u8"Content", u8"Cooked", u8"Sources", u8"Cache"})
            {
                (void)CreateDirectory(root.AsView());
                (void)CreateDirectory(PathJoin(root.AsView(), dir).AsView());
            }
            contentFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Content").AsView());
            cookedFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Cooked").AsView());
            sourcesFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Sources").AsView());
            cacheFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Cache").AsView());
            OpenDbs();

            builders.Register(UniquePtr<IAssetBuilder>(DefaultAllocator().New<CookWidgetBuilder>(),
                                                       DefaultAllocator()));
            builders.Register(UniquePtr<IAssetBuilder>(DefaultAllocator().New<ChainBuilder>(),
                                                       DefaultAllocator()));
            CookWidgetBuilder::version = 1;
            CookWidgetBuilder::fail = false;
        }

        ~Fixture() { RemoveTree(); }

        void OpenDbs()
        {
            sourceDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), *contentFs,
                                                            BinarySerializerFactory(), u8".xasset");
            cookedDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), *cookedFs,
                                                            BinarySerializerFactory(), u8".rasset");
        }

        // Re-open both DBs from disk (a fresh editor session).
        void Reopen()
        {
            sourceDb.Reset();
            cookedDb.Reset();
            OpenDbs();
        }

        [[nodiscard]] CookDriver MakeDriver()
        {
            return CookDriver(*sourceDb, *cookedDb, builders, sourcesFs.Get(), cacheFs.Get());
        }

        Guid AddWidget(StringView name, i32 quality, StringView file = {})
        {
            content::Instance* inst =
                sourceDb->RootGroup()->CreateInstance(name, CookWidgetAsset::StaticType());
            REQUIRE(inst != nullptr);
            CookWidgetAsset asset;
            asset.quality = quality;
            asset.fileName = draconic::vfs::SourcePath(file);
            REQUIRE(inst->WriteObject(asset).IsOk());
            return inst->Id();
        }

        void WriteSourceFile(StringView name, StringView text)
        {
            REQUIRE(sourcesFs->AsWritable()
                        ->Save(name, Span<const byte>(reinterpret_cast<const byte*>(text.Data()),
                                                      text.Size()))
                        .IsOk());
        }

        [[nodiscard]] i32 CookedValue(const Guid& id)
        {
            RefPtr<ISerializable> obj = cookedDb->ReadObject(id);
            auto* product = Cast<CookWidgetProduct>(obj.Get());
            return (product != nullptr) ? product->cookedValue : -1;
        }

        void RemoveTree()
        {
            // Best-effort recursive cleanup through the native mounts.
            for (StringView dir : {u8"Content", u8"Cooked", u8"Sources", u8"Cache"})
            {
                vfs::NativeFileSystem fs(PathJoin(root.AsView(), dir).AsView());
                Array<vfs::DirEntry> entries;
                if (fs.AsEnumerable()->Enumerate(u8"", entries).IsOk())
                {
                    for (const vfs::DirEntry& e : entries)
                    {
                        if (!e.isDirectory)
                        {
                            (void)fs.AsWritable()->Delete(e.name.AsView());
                        }
                    }
                }
                (void)RemoveDirectory(PathJoin(root.AsView(), dir).AsView());
            }
            (void)RemoveDirectory(root.AsView());
        }
    };

    usize PlanDirty(CookDriver& driver)
    {
        CookPlan p = driver.Plan();
        return p.dirty.Size();
    }
}

DRACONIC_DEFINE_OBJECT(CookWidgetAsset, "draconic::editor::test")
DRACONIC_DEFINE_OBJECT(CookWidgetProduct, "draconic::editor::test")
DRACONIC_DEFINE_OBJECT(ChainAsset, "draconic::editor::test")

TEST_CASE("cook: full cook then clean; products carry the source guid + product type")
{
    Fixture fx(u8"draconic_cook_test_a");
    fx.WriteSourceFile(u8"a.txt", u8"12345");
    const Guid a = fx.AddWidget(u8"A", 10, u8"a.txt");
    const Guid b = fx.AddWidget(u8"B", 20);

    CookDriver driver = fx.MakeDriver();
    CookPlan plan = driver.Plan();
    CHECK(plan.dirty.Size() == 2u);
    CHECK(plan.upToDate == 0u);

    CookStats stats = driver.Execute(plan);
    CHECK(stats.cooked == 2u);
    CHECK(stats.failed == 0u);
    // The hot-reload handoff lists exactly the rebuilt products.
    REQUIRE(stats.cookedProducts.Size() == 2u);
    bool sawA = false, sawB = false;
    for (const Guid& g : stats.cookedProducts)
    {
        sawA = sawA || g == a;
        sawB = sawB || g == b;
    }
    CHECK(sawA);
    CHECK(sawB);

    // Product guid = source guid; typed as the builder's product; content is the bake.
    content::Instance* productA = fx.cookedDb->GetInstance(a);
    REQUIRE(productA != nullptr);
    CHECK(productA->TypeName() == StringView(u8"CookWidgetProduct"));
    CHECK(fx.CookedValue(a) == 15); // quality 10 + 5 file bytes
    CHECK(fx.CookedValue(b) == 20);

    // Everything clean on the next plan.
    CookPlan again = driver.Plan();
    CHECK(again.dirty.IsEmpty());
    CHECK(again.upToDate == 2u);
}

TEST_CASE("cook: source-file edit dirties exactly the consumer; settings + version dirty too")
{
    Fixture fx(u8"draconic_cook_test_b");
    fx.WriteSourceFile(u8"a.txt", u8"12345");
    const Guid a = fx.AddWidget(u8"A", 10, u8"a.txt");
    (void)fx.AddWidget(u8"B", 20);

    CookDriver driver = fx.MakeDriver();
    CookPlan plan = driver.Plan();
    (void)driver.Execute(plan);
    REQUIRE(PlanDirty(driver) == 0u);

    // File content change (different size, so the stat memo can't false-negative).
    fx.WriteSourceFile(u8"a.txt", u8"1234567");
    CookPlan p1 = driver.Plan();
    REQUIRE(p1.dirty.Size() == 1u);
    CHECK(p1.dirty[0].source == a);
    (void)driver.Execute(p1);
    CHECK(fx.CookedValue(a) == 17);

    // Import-settings change (rewrite the instance's object -> envelope hash moves).
    {
        content::Instance* inst = fx.sourceDb->GetInstance(a);
        CookWidgetAsset asset;
        asset.quality = 11;
        asset.fileName = draconic::vfs::SourcePath(u8"a.txt");
        REQUIRE(inst->WriteObject(asset).IsOk());
    }
    CookPlan p2 = driver.Plan();
    REQUIRE(p2.dirty.Size() == 1u);
    (void)driver.Execute(p2);
    CHECK(fx.CookedValue(a) == 18);

    // Builder version bump re-cooks every product of THAT builder.
    CookWidgetBuilder::version = 2;
    CookPlan p3 = driver.Plan();
    CHECK(p3.dirty.Size() == 2u);
}

TEST_CASE("cook: read deps chain hashes + order; references never dirty their consumer")
{
    Fixture fx(u8"draconic_cook_test_c");
    fx.WriteSourceFile(u8"tex.bin", u8"xx");
    const Guid texture = fx.AddWidget(u8"Texture", 1, u8"tex.bin");

    // Material READS the texture; Scene REFERENCES the texture.
    Guid material, scene;
    {
        content::Instance* inst =
            fx.sourceDb->RootGroup()->CreateInstance(u8"Material", ChainAsset::StaticType());
        ChainAsset asset;
        asset.readDep = texture;
        REQUIRE(inst->WriteObject(asset).IsOk());
        material = inst->Id();
    }
    {
        content::Instance* inst =
            fx.sourceDb->RootGroup()->CreateInstance(u8"Scene", ChainAsset::StaticType());
        ChainAsset asset;
        asset.refDep = texture;
        REQUIRE(inst->WriteObject(asset).IsOk());
        scene = inst->Id();
    }

    CookDriver driver = fx.MakeDriver();
    CookPlan plan = driver.Plan();
    REQUIRE(plan.dirty.Size() == 3u);

    // Dependency order: the texture (level 0) cooks before the material (level 1).
    usize texIndex = 99, matIndex = 99;
    for (usize i = 0; i < plan.dirty.Size(); ++i)
    {
        if (plan.dirty[i].source == texture)
        {
            texIndex = i;
        }
        if (plan.dirty[i].source == material)
        {
            matIndex = i;
        }
    }
    CHECK(texIndex < matIndex);
    (void)driver.Execute(plan);
    REQUIRE(PlanDirty(driver) == 0u);

    // Editing the texture's file dirties texture + material (read chain), NOT the scene.
    fx.WriteSourceFile(u8"tex.bin", u8"xxxx");
    CookPlan p = driver.Plan();
    REQUIRE(p.dirty.Size() == 2u);
    bool sawScene = false;
    for (const CookItem& item : p.dirty)
    {
        sawScene = sawScene || item.source == scene;
    }
    CHECK_FALSE(sawScene);
}

TEST_CASE("cook: failures stay dirty and keep the record failed; orphans are swept")
{
    Fixture fx(u8"draconic_cook_test_d");
    const Guid a = fx.AddWidget(u8"A", 5);

    CookDriver driver = fx.MakeDriver();
    CookWidgetBuilder::fail = true;
    CookPlan plan = driver.Plan();
    CookStats stats = driver.Execute(plan);
    CHECK(stats.failed == 1u);

    // Failed cook: still dirty on the next plan (a failed record never reads clean).
    CHECK(PlanDirty(driver) == 1u);
    CookWidgetBuilder::fail = false;
    CookPlan retry = driver.Plan();
    (void)driver.Execute(retry);
    CHECK(PlanDirty(driver) == 0u);
    CHECK(fx.CookedValue(a) == 5);

    // Orphan: delete the source instance -> the plan sweeps its product + record.
    REQUIRE(fx.sourceDb->DeleteInstance(a).IsOk());
    CookPlan sweep = driver.Plan();
    REQUIRE(sweep.orphans.Size() == 1u);
    CHECK(sweep.orphans[0] == a);
    CookStats swept = driver.Execute(sweep);
    CHECK(swept.orphansSwept == 1u);
    CHECK(fx.cookedDb->GetInstance(a) == nullptr);
    CHECK(driver.Plan().orphans.IsEmpty());
}

TEST_CASE("cook: pipeline db persists across sessions; corruption degrades to a full re-plan")
{
    Fixture fx(u8"draconic_cook_test_e");
    fx.WriteSourceFile(u8"a.txt", u8"123");
    (void)fx.AddWidget(u8"A", 1, u8"a.txt");
    (void)fx.AddWidget(u8"B", 2);

    {
        CookDriver driver = fx.MakeDriver();
        CookPlan plan = driver.Plan();
        (void)driver.Execute(plan);
    }

    // A fresh session (new driver + reopened DBs) sees everything clean.
    fx.Reopen();
    {
        CookDriver driver = fx.MakeDriver();
        CookPlan plan = driver.Plan();
        CHECK(plan.dirty.IsEmpty());
        CHECK(plan.upToDate == 2u);
    }

    // Corrupt cook.db: everything re-cooks, nothing crashes, output stays correct.
    const byte garbage[7] = {byte{1}, byte{2}, byte{3}, byte{4}, byte{5}, byte{6}, byte{7}};
    REQUIRE(fx.cacheFs->AsWritable()->Save(u8"cook.db", Span<const byte>(garbage, 7)).IsOk());
    {
        CookDriver driver = fx.MakeDriver();
        CookPlan plan = driver.Plan();
        CHECK(plan.dirty.Size() == 2u);
        CookStats stats = driver.Execute(plan);
        CHECK(stats.cooked == 2u);
    }
}

// Regression (user-reported segfault): the first big PARALLEL cook (a ~30-asset model drop in
// the editor) crashed - CookItem_ created product instances on worker threads concurrently,
// racing the content DB's group tree + GUID index. Products are now pre-created serially;
// workers only read the DB. This cooks a wide level on a real JobSystem.
TEST_CASE("cook: a wide dependency level cooks in parallel on the JobSystem")
{
    Fixture fx(u8"draconic_cook_test_parallel");
    constexpr i32 kAssets = 48;
    Array<Guid> ids;
    for (i32 i = 0; i < kAssets; ++i)
    {
        String name(u8"W");
        name.PushBack(static_cast<utf8char>('a' + i % 26));
        name.PushBack(static_cast<utf8char>('a' + (i / 26) % 26));
        ids.PushBack(fx.AddWidget(name.AsView(), i));
    }

    JobSystem jobs;
    CookDriver driver(*fx.sourceDb, *fx.cookedDb, fx.builders, fx.sourcesFs.Get(), fx.cacheFs.Get(),
                      &jobs);

    CookPlan plan = driver.Plan();
    REQUIRE(plan.dirty.Size() == static_cast<usize>(kAssets));
    CookStats stats = driver.Execute(plan);
    CHECK(stats.cooked == static_cast<usize>(kAssets));
    CHECK(stats.failed == 0u);

    // Every product exists with the right content; the follow-up plan is clean.
    for (i32 i = 0; i < kAssets; ++i)
    {
        CHECK(fx.CookedValue(ids[static_cast<usize>(i)]) == i);
    }
    CHECK(driver.Plan().dirty.IsEmpty());
}
