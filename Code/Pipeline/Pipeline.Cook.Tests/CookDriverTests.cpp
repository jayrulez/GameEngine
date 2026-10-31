// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// CookDriver headless tests: the recipe-hash staleness model end to end. The fixture is four
// NativeFileSystem mounts (source DB, cooked DB, sources, cache) under a temp tree, one
// file-reading builder and one dependency-declaring builder.

#include <doctest/doctest.h>

#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"
#include <initializer_list>

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.cook;

using namespace foundation::core;
    using namespace pipeline;
namespace content = foundation::content;
namespace vfs = foundation::vfs;

namespace
{
    // === Test asset types ===

    class CookWidgetAsset final : public Asset
    {
        RTTI_OBJECT(CookWidgetAsset, Asset)
    public:
        i32 quality = 1;
        void Serialize(ISerializer& ar) override
        {
            Asset::Serialize(ar);
            foundation::core::Serialize(ar, "quality", quality);
        }
    };

    class CookWidgetProduct final : public ISerializable
    {
        RTTI_OBJECT(CookWidgetProduct, ISerializable)
    public:
        i32 cookedValue = 0;
        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "cookedValue", cookedValue);
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

    // An asset that reads one instance and references another (the two dependency kinds).
    class ChainAsset final : public Asset
    {
        RTTI_OBJECT(ChainAsset, Asset)
    public:
        Guid readDep;
        Guid refDep;
        void Serialize(ISerializer& ar) override
        {
            Asset::Serialize(ar);
            foundation::core::Serialize(ar, "readDep", readDep);
            foundation::core::Serialize(ar, "refDep", refDep);
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

    // A platform-variant asset: its product depends on the export target, so it
    // cooks per target and is salted in the recipe, never copied forward. The product's cookedValue
    // encodes the target (999 for astc, else 100) beside a data sidecar, so a test can see it recooked.
    class VariantAsset final : public Asset
    {
        RTTI_OBJECT(VariantAsset, Asset)
    public:
        void Serialize(ISerializer& ar) override { Asset::Serialize(ar); }
    };

    class VariantBuilder final : public DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const TypeInfo* AssetType() const override
        {
            return &VariantAsset::StaticType();
        }
        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &CookWidgetProduct::StaticType();
        }
        [[nodiscard]] BuildVariance Variance() const override
        {
            return BuildVariance::PlatformVariant;
        }
        [[nodiscard]] Status Build(const Asset&, AssetBuildContext& ctx) override
        {
            CookWidgetProduct product;
            product.cookedValue = (ctx.target != nullptr && ctx.target->astc) ? 999 : 100;
            const Status wrote = ctx.output->WriteObject(product);
            if (!wrote.IsOk())
            {
                return wrote;
            }
            // A data sidecar too, so copy-forward's stream copy is exercised for the invariant case.
            const byte payload[1] = {static_cast<byte>(product.cookedValue)};
            return ctx.output->WriteData(u8"data", Span<const byte>(payload, 1));
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
        GlobalTypeRegistry().Register(VariantAsset::StaticType());
        RegisterSerializable<CookWidgetAsset>();
        RegisterSerializable<CookWidgetProduct>();
        RegisterSerializable<ChainAsset>();
        RegisterSerializable<VariantAsset>();
    }

    // === Fixture: a temp project tree with the four mounts ===

    struct Fixture
    {
        String root;
        UniquePtr<vfs::NativeFileSystem> contentFs, cookedFs, sourcesFs, cacheFs;
        UniquePtr<content::ContentDatabase> sourceDb, cookedDb;
        BuilderRegistry builders{DefaultAllocator()};

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
                DefaultAllocator(), PathJoin(root.AsView(), u8"Content").AsView(), DefaultAllocator());
            cookedFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Cooked").AsView(), DefaultAllocator());
            sourcesFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Sources").AsView(), DefaultAllocator());
            cacheFs = MakeUnique<vfs::NativeFileSystem>(
                DefaultAllocator(), PathJoin(root.AsView(), u8"Cache").AsView(), DefaultAllocator());
            OpenDbs();

            builders.Register(UniquePtr<IAssetBuilder>(DefaultAllocator().New<CookWidgetBuilder>(),
                                                       DefaultAllocator()));
            builders.Register(UniquePtr<IAssetBuilder>(DefaultAllocator().New<ChainBuilder>(),
                                                       DefaultAllocator()));
            builders.Register(UniquePtr<IAssetBuilder>(DefaultAllocator().New<VariantBuilder>(),
                                                       DefaultAllocator()));
            CookWidgetBuilder::version = 1;
            CookWidgetBuilder::fail = false;
        }

        ~Fixture() { RemoveTree(); }

        void OpenDbs()
        {
            sourceDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *contentFs,
                                                            BinarySerializerFactory(), u8".xasset");
            cookedDb = MakeUnique<content::ContentDatabase>(DefaultAllocator(), DefaultAllocator(), *cookedFs,
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
            return CookDriver(DefaultAllocator(), *sourceDb, *cookedDb, builders,
                              sourcesFs.Get(), cacheFs.Get());
        }

        Guid AddWidget(StringView name, i32 quality, StringView file = {})
        {
            content::Instance* inst =
                sourceDb->RootGroup()->CreateInstance(name, CookWidgetAsset::StaticType());
            REQUIRE(inst != nullptr);
            CookWidgetAsset asset;
            asset.quality = quality;
            asset.fileName = foundation::vfs::SourcePath(file);
            REQUIRE(inst->WriteObject(asset).IsOk());
            return inst->Id();
        }

        Guid AddVariant(StringView name)
        {
            content::Instance* inst =
                sourceDb->RootGroup()->CreateInstance(name, VariantAsset::StaticType());
            REQUIRE(inst != nullptr);
            VariantAsset asset;
            REQUIRE(inst->WriteObject(asset).IsOk());
            return inst->Id();
        }

        Guid AddChain(StringView name, const Guid& readDep, const Guid& refDep = {})
        {
            content::Instance* inst =
                sourceDb->RootGroup()->CreateInstance(name, ChainAsset::StaticType());
            REQUIRE(inst != nullptr);
            ChainAsset asset;
            asset.readDep = readDep;
            asset.refDep = refDep;
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
                vfs::NativeFileSystem fs(PathJoin(root.AsView(), dir).AsView(), DefaultAllocator());
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

RTTI_DEFINE_OBJECT(CookWidgetAsset, "rtti::pipeline::editor::test")
RTTI_DEFINE_OBJECT(CookWidgetProduct, "rtti::pipeline::editor::test")
RTTI_DEFINE_OBJECT(ChainAsset, "rtti::pipeline::editor::test")
RTTI_DEFINE_OBJECT(VariantAsset, "rtti::pipeline::editor::test")

TEST_CASE("cook: full cook then clean; products carry the source guid + product type")
{
    Fixture fx(u8"scratch_cook_test_a");
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
    CHECK(fx.CookedValue(a) == 15); // quality 10 plus 5 file bytes
    CHECK(fx.CookedValue(b) == 20);

    // Everything clean on the next plan.
    CookPlan again = driver.Plan();
    CHECK(again.dirty.IsEmpty());
    CHECK(again.upToDate == 2u);
}

TEST_CASE("cook: source-file edit dirties exactly the consumer; settings + version dirty too")
{
    Fixture fx(u8"scratch_cook_test_b");
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

    // Import-settings change (rewriting the instance's object moves the envelope hash).
    {
        content::Instance* inst = fx.sourceDb->GetInstance(a);
        CookWidgetAsset asset;
        asset.quality = 11;
        asset.fileName = foundation::vfs::SourcePath(u8"a.txt");
        REQUIRE(inst->WriteObject(asset).IsOk());
    }
    CookPlan p2 = driver.Plan();
    REQUIRE(p2.dirty.Size() == 1u);
    (void)driver.Execute(p2);
    CHECK(fx.CookedValue(a) == 18);

    // A builder version bump re-cooks every product of that builder.
    CookWidgetBuilder::version = 2;
    CookPlan p3 = driver.Plan();
    CHECK(p3.dirty.Size() == 2u);
}

TEST_CASE("cook: read deps chain hashes + order; references never dirty their consumer")
{
    Fixture fx(u8"scratch_cook_test_c");
    fx.WriteSourceFile(u8"tex.bin", u8"xx");
    const Guid texture = fx.AddWidget(u8"Texture", 1, u8"tex.bin");

    // Material reads the texture; Scene references the texture.
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

    // Editing the texture's file dirties the texture and material (read chain), not the scene.
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
    Fixture fx(u8"scratch_cook_test_d");
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

    // Orphan: delete the source instance, and the plan sweeps its product and record.
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
    Fixture fx(u8"scratch_cook_test_e");
    fx.WriteSourceFile(u8"a.txt", u8"123");
    (void)fx.AddWidget(u8"A", 1, u8"a.txt");
    (void)fx.AddWidget(u8"B", 2);

    {
        CookDriver driver = fx.MakeDriver();
        CookPlan plan = driver.Plan();
        (void)driver.Execute(plan);
    }

    // A fresh session (new driver, reopened DBs) sees everything clean.
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

// A wide dependency level cooks in parallel on a real JobSystem. Product instances are
// pre-created serially, because creating them on worker threads would race the content DB's
// group tree and GUID index; workers only read the DB.
TEST_CASE("cook: a wide dependency level cooks in parallel on the JobSystem")
{
    Fixture fx(u8"scratch_cook_test_parallel");
    constexpr i32 kAssets = 48;
    Array<Guid> ids;
    for (i32 i = 0; i < kAssets; ++i)
    {
        String name(u8"W");
        name.PushBack(static_cast<utf8char>('a' + i % 26));
        name.PushBack(static_cast<utf8char>('a' + (i / 26) % 26));
        ids.PushBack(fx.AddWidget(name.AsView(), i));
    }

    JobSystem jobs(DefaultAllocator());
    CookDriver driver(DefaultAllocator(), *fx.sourceDb, *fx.cookedDb, fx.builders, fx.sourcesFs.Get(), fx.cacheFs.Get(),
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

TEST_CASE("cook variant axis: target cook recooks variant products + copies invariant ones forward")
{
    // Host cook (BC desktop), then a "web-astc" target cook into a separate DB:
    // the platform-variant product recooks with the target profile (100 becomes 999); the
    // invariant products are carried forward from the host DB, never rebuilt (the cook-stats assertion).
    Fixture fx(u8"scratch_cook_variant");
    fx.WriteSourceFile(u8"a.txt", u8"12345");
    const Guid inv1 = fx.AddWidget(u8"Inv1", 10, u8"a.txt"); // invariant (file-backed)
    const Guid inv2 = fx.AddWidget(u8"Inv2", 20);            // invariant
    const Guid var = fx.AddVariant(u8"Var");                 // platform-variant

    // --- host cook (the always-warm desktop DB) ---
    CookDriver host = fx.MakeDriver();
    CookPlan hostPlan = host.Plan();
    REQUIRE(hostPlan.dirty.Size() == 3u);
    CookStats hostStats = host.Execute(hostPlan);
    CHECK(hostStats.cooked == 3u);
    CHECK(hostStats.copiedForward == 0u);
    CHECK(fx.CookedValue(var) == 100); // host: BC profile (astc=false)

    // --- a per-target cooked DB and its own cook cache ---
    (void)CreateDirectory(PathJoin(fx.root.AsView(), u8"Cooked-astc").AsView());
    (void)CreateDirectory(PathJoin(fx.root.AsView(), u8"Cache-astc").AsView());
    vfs::NativeFileSystem targetCookedFs(PathJoin(fx.root.AsView(), u8"Cooked-astc").AsView(), DefaultAllocator());
    vfs::NativeFileSystem targetCacheFs(PathJoin(fx.root.AsView(), u8"Cache-astc").AsView(), DefaultAllocator());
    content::ContentDatabase targetDb(DefaultAllocator(), targetCookedFs, BinarySerializerFactory(), u8".rasset");

    CookTarget astc{String(u8"web-astc"), /*bc*/ false, /*astc*/ true, /*etc2*/ false};
    CookDriver target(DefaultAllocator(), *fx.sourceDb, targetDb, fx.builders, fx.sourcesFs.Get(), &targetCacheFs);
    target.SetTarget(astc);
    target.SetCopyForwardSource(*fx.cookedDb, host.Db());

    CookPlan targetPlan = target.Plan();
    REQUIRE(targetPlan.dirty.Size() == 3u); // fresh DB: everything is "dirty"
    CookStats targetStats = target.Execute(targetPlan);

    // The key property: exactly the one variant product cooked; the two invariant ones were
    // copied forward, not rebuilt.
    CHECK(targetStats.cooked == 1u);
    CHECK(targetStats.copiedForward == 2u);
    CHECK(targetStats.failed == 0u);

    // The variant product differs per target (ASTC branch); invariants match the host byte for byte.
    auto targetValue = [&](const Guid& id)
    {
        RefPtr<ISerializable> obj = targetDb.ReadObject(id);
        auto* p = Cast<CookWidgetProduct>(obj.Get());
        return (p != nullptr) ? p->cookedValue : -1;
    };
    CHECK(targetValue(var) == 999);          // recooked with the ASTC profile
    CHECK(targetValue(inv1) == fx.CookedValue(inv1)); // copied forward unchanged
    CHECK(targetValue(inv2) == fx.CookedValue(inv2));

    // The copied-forward invariant sidecar came across too (Var's "data" stream).
    UniquePtr<IStream> vdata = targetDb.GetInstance(var)->ReadData(u8"data");
    REQUIRE(vdata);
    CHECK(vdata->Size() == 1);

    // A second target cook is fully clean: nothing recooks, nothing re-copies.
    CHECK(target.Plan().dirty.IsEmpty());
}

TEST_CASE("cook variant axis: an invariant product that READS variant content recooks per target")
{
    // The salt must propagate through read-deps (CookDriverImpl folds each read's recipe). An
    // invariant builder whose product reads a variant product's content is effectively variant: its
    // recipe differs per target, so copy-forward must decline and it recooks; otherwise the target
    // DB would inherit host-BC-derived bytes.
    Fixture fx(u8"scratch_cook_variant_readchain");
    const Guid var = fx.AddVariant(u8"Var");         // platform-variant
    (void)fx.AddChain(u8"Reader", var);              // invariant builder, reads the variant
    const Guid plain = fx.AddWidget(u8"Plain", 7);   // invariant, reads nothing: copies forward

    CookDriver host = fx.MakeDriver();
    CookPlan hostPlan = host.Plan();
    CookStats hostStats = host.Execute(hostPlan);
    REQUIRE(hostStats.failed == 0u);
    REQUIRE(hostStats.cooked == 3u);

    (void)CreateDirectory(PathJoin(fx.root.AsView(), u8"Cooked-astc").AsView());
    (void)CreateDirectory(PathJoin(fx.root.AsView(), u8"Cache-astc").AsView());
    vfs::NativeFileSystem targetCookedFs(PathJoin(fx.root.AsView(), u8"Cooked-astc").AsView(), DefaultAllocator());
    vfs::NativeFileSystem targetCacheFs(PathJoin(fx.root.AsView(), u8"Cache-astc").AsView(), DefaultAllocator());
    content::ContentDatabase targetDb(DefaultAllocator(), targetCookedFs, BinarySerializerFactory(), u8".rasset");

    CookTarget astc{String(u8"web-astc"), false, true, false};
    CookDriver target(DefaultAllocator(), *fx.sourceDb, targetDb, fx.builders, fx.sourcesFs.Get(), &targetCacheFs);
    target.SetTarget(astc);
    target.SetCopyForwardSource(*fx.cookedDb, host.Db());

    CookPlan targetPlan = target.Plan();
    CookStats targetStats = target.Execute(targetPlan);

    // Var (variant) and Reader (reads var, salted recipe mismatch) both recook; only Plain copies.
    CHECK(targetStats.cooked == 2u);
    CHECK(targetStats.copiedForward == 1u);
    CHECK(targetStats.failed == 0u);

    // Plain (no variant input) came across unchanged.
    RefPtr<ISerializable> plainObj = targetDb.ReadObject(plain);
    auto* plainProduct = Cast<CookWidgetProduct>(plainObj.Get());
    REQUIRE(plainProduct != nullptr);
    CHECK(plainProduct->cookedValue == fx.CookedValue(plain));
}
