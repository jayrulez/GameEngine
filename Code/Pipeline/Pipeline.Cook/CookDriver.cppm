// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Pipeline::Cook - the `editor.cook` module.
//
// The incremental cook driver. One rule decides everything: an asset's recipe hash is
// H(source envelope bytes, each source file's content, builder version, recipe of each
// content-read dependency), folded in order into a single u64, and an asset is dirty when its
// product is missing or the hash does not match. Deterministic across machines and checkouts:
// content hashes decide, and stat (size, mtime) is only a memo to skip re-hashing untouched
// files, persisted in the pipeline DB at .cache/cook.db.
//
// Plan() walks the source DB, routes instances to builders, and computes recipes (memoized file
// hashes, read-dependency chaining with a cycle guard), giving the dirty set in dependency order
// and an orphan sweep (records whose source is gone have their products deleted).
// Execute() cooks the dirty items, in parallel within dependency levels on the JobSystem,
// writing products into the cooked DB with the product guid equal to the source guid, then
// persists the pipeline DB. A failed build keeps the last good product and marks the record failed.
//
// The pipeline DB is one binary file, written whole through the cache mount; corruption or a
// version mismatch degrades to a full re-plan: never wrong output, only wasted work.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module pipeline.cook;

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;

using namespace foundation::core;

export namespace pipeline
{
    namespace content = foundation::content;
    namespace vfs = foundation::vfs;

    // === Pipeline DB ===

    /// Per-file memo: content hash and the stat that validated it.
    struct CookFileMemo
    {
        String path; // sources-mount-relative
        u64 size = 0;
        i64 modifiedTime = 0;
        u64 contentHash = 0;
    };

    /// One source asset's last cook.
    struct CookRecord
    {
        Guid source;
        u64 recipeHash = 0;
        bool failed = false;
        Array<CookFileMemo> files;
        Array<Guid> reads;
        Array<Guid> references;
    };

    inline void Serialize(ISerializer& ar, CookFileMemo& m)
    {
        foundation::core::Serialize(ar, "path", m.path);
        foundation::core::Serialize(ar, "size", m.size);
        foundation::core::Serialize(ar, "mtime", m.modifiedTime);
        foundation::core::Serialize(ar, "hash", m.contentHash);
    }

    /// The persisted pipeline state (.cache/cook.db): source Guid -> CookRecord.
    class CookDb
    {
    public:
        // The allocator (required; the owner decides) backs the cook records.
        explicit CookDb(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        static constexpr u32 kVersion = 1;
        // Annotated on the member, not the class: dllexport on a class emits every
        // implicit member (CookDb holds move-only UniquePtr storage, so its copy constructor is
        // deleted) and flags each non-exported member type C4251.
        static constexpr ENGINE_EXPORT_DATA StringView kDefaultName = u8"cook.db";

        [[nodiscard]] CookRecord* Find(const Guid& source);

        CookRecord& Upsert(const Guid& source);

        void Remove(const Guid& source);

        void ForEach(const Function<void(const CookRecord&)>& fn) const;

        [[nodiscard]] usize Count() const noexcept { return m_storage.Size(); }

        /// Load from the cache mount. A missing, corrupt or mismatched file gives an empty DB (full re-plan).
        void Load(vfs::IFileSystem& cache, StringView name = kDefaultName);

        [[nodiscard]] Status Save(vfs::IFileSystem& cache, StringView name = kDefaultName) const;

    private:
        static void SerializeRecord(ISerializer& ar, CookRecord& r);
        IAllocator* m_allocator;

        HashMap<Guid, CookRecord*> m_records;
        Array<UniquePtr<CookRecord>> m_storage;
    };

    // === Recipe hashing ===

    namespace detail
    {
        // Ordered fold: every field is tagged so reorderings and offsets can't cancel out,
        // as they can in a commutative sum.
        inline u64 FoldHash(u64 seed, u64 tag, u64 value)
        {
            u64 h = seed;
            h = HashBytes(&tag, sizeof(tag), h);
            h = HashBytes(&value, sizeof(value), h);
            return h;
        }

        inline u64 HashStream(IStream& stream)
        {
            byte chunk[64 * 1024];
            u64 h = HashBytes(nullptr, 0);
            for (;;)
            {
                const u64 read = stream.Read(chunk, sizeof(chunk));
                if (read == 0)
                {
                    break;
                }
                h = HashBytes(chunk, static_cast<usize>(read), h);
            }
            return h;
        }
    }

    // === Plan and Execute ===

    struct CookItem
    {
        Guid source;
        String path; // source instance path (progress display and product placement)
        IAssetBuilder* builder = nullptr;
        RefPtr<ISerializable> asset; // deserialized source object (kept for Build)
        u64 recipeHash = 0;
        AssetDependencies deps;
        i32 level = 0; // dependency depth (items cook level-by-level, parallel within)
        foundation::content::Instance* product = nullptr; // pre-created serially before workers run
        foundation::content::Instance* sourceInstance = nullptr; // snapshotted in PrepareProducts
        bool copiedForward = false; // filled in PrepareProducts: invariant product carried from host
    };

    struct CookPlan
    {
        Array<CookItem> dirty;       // in dependency order (level ascending)
        Array<Guid> orphans;         // records whose source is gone; their products are swept
        usize orphansSweptCount = 0; // filled by PrepareProducts
        usize upToDate = 0;
        usize unbuildable = 0; // instances with no registered builder (informational)
        Array<Guid> reachable; // PlanFor only: the roots and their whole dependency closure (every
                               // visited guid, clean or dirty, buildable or not), the reachable
                               // set export pruning ships. Empty for a whole-project Plan().
    };

    struct CookProgress
    {
        // (done, total, sourcePath, ok) after each item completes.
        Function<void(usize, usize, StringView, bool)> onItem;
    };

    struct CookStats
    {
        usize cooked = 0;
        usize failed = 0;
        usize copiedForward = 0;    // invariant products carried from the host DB (variant cook)
        usize orphansSwept = 0;     // (CookPlan carries the swept count from PrepareProducts)
        Array<Guid> cookedProducts; // successfully (re)built or copied products, for hot reload
    };

    class CookDriver
    {
    public:
        CookDriver(IAllocator& allocator, content::ContentDatabase& sourceDb,
                   content::ContentDatabase& cookedDb, BuilderRegistry& builders,
                   vfs::IFileSystem* sourcesMount, vfs::IFileSystem* cacheMount,
                   JobSystem* jobs = nullptr)
            : m_allocator(&allocator), m_sourceDb(&sourceDb), m_cookedDb(&cookedDb),
              m_builders(&builders),
              m_sources(sourcesMount), m_cache(cacheMount), m_jobs(jobs)
        {
            if (m_cache != nullptr)
            {
                m_db.Load(*m_cache);
            }
        }

        [[nodiscard]] CookDb& Db() noexcept { return m_db; }

        /// The export target this driver cooks for (default HostTarget()).
        /// Salts the recipe of variant builders (so a texture recooks per target) and is handed to
        /// every Build() through AssetBuildContext::target. Set before Plan() and Execute().
        void SetTarget(const CookTarget& target) { m_target = target; }
        [[nodiscard]] const CookTarget& Target() const noexcept { return m_target; }

        /// Enable platform-invariant copy-forward: when cooking a per-target DB,
        /// an invariant product whose recipe matches the host DB's record is copied from `hostCookedDb`
        /// instead of being recooked. `hostRecords` is the host DB's already-loaded cook.db (its
        /// recipe hashes gate the copy). Leave unset (the default) for a normal single-DB cook.
        void SetCopyForwardSource(content::ContentDatabase& hostCookedDb, CookDb& hostRecords)
        {
            m_hostCookedDb = &hostCookedDb;
            m_hostRecords = &hostRecords;
        }

        /// Scoped plan: the requested roots plus their dependency closure (reads and
        /// references, transitively, so a material's textures cook with it). `force` re-cooks
        /// the roots regardless of cleanliness; closure deps keep the normal clean check.
        /// No orphan sweep (that is a whole-project concern). Lets huge projects cook one
        /// group or asset at a time instead of everything at once.
        [[nodiscard]] CookPlan PlanFor(Span<const Guid> roots, bool force = false);

        /// Compute the dirty set and orphans. `force` marks every buildable instance dirty.
        [[nodiscard]] CookPlan Plan(bool force = false);

    private:
        /// Scan one source instance and append it to the plan when dirty (or forced).
        /// `outDeps` (optional) receives its dependencies even when clean, since PlanFor walks
        /// the closure through clean items too.
        void PlanInstance(content::Instance& instanceRef, bool force, CookPlan& plan,
                          HashMap<Guid, i32>& levels, AssetDependencies* outDeps);

    public:
        /// Cook the plan. Items run level-by-level; within a level in parallel when a
        /// JobSystem was provided. Persists the pipeline DB at the end.
        /// Single-threaded callers (CLI, tests): Prepare and builds back to back.
        CookStats Execute(CookPlan& plan, const CookProgress* progress = nullptr);

        /// Phase 1, which must run on the thread that owns the content DBs (the editor's main
        /// thread): sweeps orphans, pre-creates product instances, and snapshots the source
        /// instance pointers the builds need. The DBs' group trees and GUID indices are not
        /// thread-safe; doing any of this on the cook worker races main-thread imports,
        /// deletes, and resource loads (planning or sweeping on the worker while the main thread
        /// mutates the DBs produces garbage deserialization).
        void PrepareProducts(CookPlan& plan);

        /// Phase 2, worker-safe: builds only. No DB queries (every instance pointer was
        /// snapshotted by PrepareProducts); workers read source/product instances they were
        /// handed and write their own product's files.
        CookStats ExecuteBuilds(CookPlan& plan, const CookProgress* progress = nullptr);

    private:
        static void CollectInstances(content::Group* group, Array<content::Instance*>& out);

        // Content hash of one source file through the mount, memoized by (size, mtime) against
        // the previous record when the mount supports stat.
        [[nodiscard]] u64 HashSourceFile(StringView path, const CookRecord* previous,
                                         Array<CookFileMemo>& outMemos);

        // The recipe hash: envelope bytes, file contents, builder version and
        // read-dependency recipes, folded in order. Guarded against read-dependency cycles.
        [[nodiscard]] u64 ComputeRecipe(content::Instance& instance, const Asset& asset,
                                        IAssetBuilder& builder, const AssetDependencies& deps,
                                        i32 depth);

        // Depth of the read-dependency chain (level 0 cooks first).
        [[nodiscard]] i32 ReadDepth(const Guid& source, const AssetDependencies& deps,
                                    HashMap<Guid, i32>& levels, i32 depth);

        static void SortByLevel(Array<CookItem>& items);

        static void SortGuids(Array<Guid>& guids);

        // The product instance for an item: mirrored path, same guid, stamped with the
        // builder's product type. Mutates the cooked DB, so main thread only (see Execute).
        [[nodiscard]] content::Instance* EnsureProduct(const CookItem& item);

        // Cook one item into its pre-created product and update its record. Runs on worker
        // threads: no DB mutation here, only reads and the product's own file writes.
        [[nodiscard]] bool CookItem_(CookItem& item);

        // Copy-forward (main thread, in PrepareProducts): if `item` is an invariant product whose
        // host record recipe matches, copy the host product into item.product and stamp the target
        // record. Returns true when carried forward (the item then skips the build).
        [[nodiscard]] bool TryCopyForward(CookItem& item);

        // The cooked-DB group mirroring the source instance's group path.
        [[nodiscard]] content::Group* MirrorGroup(content::Group& sourceGroup);
        IAllocator* m_allocator;

        content::ContentDatabase* m_sourceDb;
        content::ContentDatabase* m_cookedDb;
        BuilderRegistry* m_builders;
        vfs::IFileSystem* m_sources; // nullable: embedded-data projects have no source files
        vfs::IFileSystem* m_cache;   // nullable: no persistence (tests / one-shot cooks)
        JobSystem* m_jobs;           // nullable: serial execution

        CookTarget m_target = HostTarget(); // the export target this driver cooks for
        content::ContentDatabase* m_hostCookedDb = nullptr; // copy-forward source; null = off
        CookDb* m_hostRecords = nullptr;                    // host cook.db; gates the copy by recipe

        CookDb m_db{*m_allocator};
        HashMap<Guid, u64> m_recipeMemo;                   // per-Plan recipe cache
        HashMap<Guid, Array<CookFileMemo>> m_pendingMemos; // file memos gathered during Plan
        Mutex m_recordMutex;                               // record updates from worker threads
    };

    // The reusable per-target cook step: cook `sourceDb` for `target` into
    // `targetCookedDb`, carrying platform-invariant products forward from an already-cooked
    // `hostCookedDb` (gated by `hostRecords`) instead of recooking them. `targetCache` is the target's
    // own cook.db mount. Both Tools.Cook --target and the web export drive this. `force` re-cooks all.
    [[nodiscard]] inline CookStats CookForTarget(
        IAllocator& allocator, content::ContentDatabase& sourceDb,
        content::ContentDatabase& targetCookedDb,
        content::ContentDatabase& hostCookedDb, CookDb& hostRecords, BuilderRegistry& builders,
        vfs::IFileSystem* sources, vfs::IFileSystem* targetCache, const CookTarget& target,
        JobSystem* jobs = nullptr, bool force = false)
    {
        CookDriver driver(allocator, sourceDb, targetCookedDb, builders, sources, targetCache,
                          jobs);
        driver.SetTarget(target);
        driver.SetCopyForwardSource(hostCookedDb, hostRecords);
        CookPlan plan = driver.Plan(force);
        return driver.Execute(plan);
    }
}
