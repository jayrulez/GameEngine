// Draconic::EditorCook - the `draconic.editor.cook` module.
//
// The incremental cook driver (docs/design/asset-pipeline.md §3/§5). One rule decides
// everything: an asset's RECIPE HASH = H(source envelope bytes, each source file's content,
// builder version, recipe of each content-READ dependency), folded ORDERED into a single u64 -
// dirty <=> product missing or hash mismatch. Deterministic across machines/checkouts (content
// hashes decide; stat (size,mtime) is only a memo to skip re-hashing untouched files, persisted
// in the pipeline DB at .cache/cook.db).
//
// Plan(): walk the source DB -> route instances to builders -> compute recipes (memoized file
// hashes, read-dep chaining with cycle guard) -> dirty set in dependency order + orphan sweep
// (records whose source is gone -> their products are deleted; Traktor's missing piece).
// Execute(): cook dirty items - parallel within dependency levels on the JobSystem - writing
// products into the cooked DB with PRODUCT GUID = SOURCE GUID (user-confirmed), then persist
// the pipeline DB. A failed build keeps the last good product and marks the record failed.
//
// The pipeline DB is one binary file, written whole through the cache mount; corruption or a
// version mismatch degrades to a full re-plan - never wrong output, only wasted work.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

export module draconic.editor.cook;

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.editor;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace content = draconic::content;
    namespace vfs = draconic::vfs;

    // === Pipeline DB ===

    /// Per-file memo: content hash + the stat that validated it.
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
        draconic::foundation::Serialize(ar, "path", m.path);
        draconic::foundation::Serialize(ar, "size", m.size);
        draconic::foundation::Serialize(ar, "mtime", m.modifiedTime);
        draconic::foundation::Serialize(ar, "hash", m.contentHash);
    }

    /// The persisted pipeline state (.cache/cook.db): source Guid -> CookRecord.
    class CookDb
    {
    public:
        static constexpr u32 kVersion = 1;
        static constexpr StringView kDefaultName = u8"cook.db";

        [[nodiscard]] CookRecord* Find(const Guid& source);

        CookRecord& Upsert(const Guid& source);

        void Remove(const Guid& source);

        void ForEach(const Function<void(const CookRecord&)>& fn) const;

        [[nodiscard]] usize Count() const noexcept { return m_storage.Size(); }

        /// Load from the cache mount. Missing/corrupt/version-mismatch = empty DB (full re-plan).
        void Load(vfs::IFileSystem& cache, StringView name = kDefaultName);

        [[nodiscard]] Status Save(vfs::IFileSystem& cache, StringView name = kDefaultName) const;

    private:
        static void SerializeRecord(ISerializer& ar, CookRecord& r);

        HashMap<Guid, CookRecord*> m_records;
        Array<UniquePtr<CookRecord>> m_storage;
    };

    // === Recipe hashing ===

    namespace detail
    {
        // Ordered fold: every field is tagged so reorderings/offsets can't cancel out
        // (Traktor's commutative sums are the cautionary tale).
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

    // === Plan / Execute ===

    struct CookItem
    {
        Guid source;
        String path; // source instance path (progress display + product placement)
        IAssetBuilder* builder = nullptr;
        RefPtr<ISerializable> asset; // deserialized source object (kept for Build)
        u64 recipeHash = 0;
        AssetDependencies deps;
        i32 level = 0; // dependency depth (items cook level-by-level, parallel within)
        draconic::content::Instance* product = nullptr; // pre-created SERIALLY before workers run
        draconic::content::Instance* sourceInstance = nullptr; // snapshotted in PrepareProducts
    };

    struct CookPlan
    {
        Array<CookItem> dirty;       // in dependency order (level ascending)
        Array<Guid> orphans;         // records whose source is gone -> products swept
        usize orphansSweptCount = 0; // filled by PrepareProducts
        usize upToDate = 0;
        usize unbuildable = 0; // instances with no registered builder (informational)
        Array<Guid> reachable; // PlanFor only: the roots + their whole dependency CLOSURE (every
                               // visited guid, clean or dirty, buildable or not) - the reachable
                               // SET export pruning ships. Empty for a whole-project Plan().
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
        usize orphansSwept = 0;     // (CookPlan carries the swept count from PrepareProducts)
        Array<Guid> cookedProducts; // successfully (re)built products - hot-reload input
    };

    class CookDriver
    {
    public:
        CookDriver(content::ContentDatabase& sourceDb, content::ContentDatabase& cookedDb,
                   BuilderRegistry& builders, vfs::IFileSystem* sourcesMount,
                   vfs::IFileSystem* cacheMount, JobSystem* jobs = nullptr)
            : m_sourceDb(&sourceDb), m_cookedDb(&cookedDb), m_builders(&builders),
              m_sources(sourcesMount), m_cache(cacheMount), m_jobs(jobs)
        {
            if (m_cache != nullptr)
            {
                m_db.Load(*m_cache);
            }
        }

        [[nodiscard]] CookDb& Db() noexcept { return m_db; }

        /// Scoped plan: the requested roots plus their dependency CLOSURE (reads +
        /// references, transitively - a material's textures cook with it). `force` re-cooks
        /// the ROOTS regardless of cleanliness; closure deps keep the normal clean check.
        /// No orphan sweep (that is a whole-project concern). Lets huge projects cook one
        /// group/asset at a time instead of everything at once.
        [[nodiscard]] CookPlan PlanFor(Span<const Guid> roots, bool force = false);

        /// Compute the dirty set (+ orphans). `force` marks every buildable instance dirty.
        [[nodiscard]] CookPlan Plan(bool force = false);

    private:
        /// Scan one source instance and append it to the plan when dirty (or forced).
        /// `outDeps` (optional) receives its dependencies even when clean - PlanFor walks
        /// the closure through clean items too.
        void PlanInstance(content::Instance& instanceRef, bool force, CookPlan& plan,
                          HashMap<Guid, i32>& levels, AssetDependencies* outDeps);

    public:
        /// Cook the plan. Items run level-by-level; within a level in parallel when a
        /// JobSystem was provided. Persists the pipeline DB at the end.
        /// Single-threaded callers (CLI, tests): Prepare + builds back to back.
        CookStats Execute(CookPlan& plan, const CookProgress* progress = nullptr);

        /// Phase 1 - MUST run on the thread that owns the content DBs (the editor's main
        /// thread): sweeps orphans, pre-creates product instances, and snapshots the source
        /// instance pointers the builds need. The DBs' group trees and GUID indices are not
        /// thread-safe; doing ANY of this on the cook worker races main-thread imports,
        /// deletes, and resource loads (the delete->reimport crash: Plan/sweep on the worker
        /// while the main thread mutated the DBs produced garbage deserialization).
        void PrepareProducts(CookPlan& plan);

        /// Phase 2 - worker-safe: builds only. No DB queries (every instance pointer was
        /// snapshotted by PrepareProducts); workers read source/product instances they were
        /// handed and write their own product's files.
        CookStats ExecuteBuilds(CookPlan& plan, const CookProgress* progress = nullptr);

    private:
        static void CollectInstances(content::Group* group, Array<content::Instance*>& out);

        // Content hash of one source file through the mount, memoized by (size, mtime) against
        // the previous record when the mount supports stat.
        [[nodiscard]] u64 HashSourceFile(StringView path, const CookRecord* previous,
                                         Array<CookFileMemo>& outMemos);

        // The recipe hash (design §3): envelope bytes + file contents + builder version +
        // read-dep recipes, folded ordered. Guarded against read-dep cycles.
        [[nodiscard]] u64 ComputeRecipe(content::Instance& instance, const Asset& asset,
                                        IAssetBuilder& builder, const AssetDependencies& deps,
                                        i32 depth);

        // Depth of the read-dependency chain (level 0 cooks first).
        [[nodiscard]] i32 ReadDepth(const Guid& source, const AssetDependencies& deps,
                                    HashMap<Guid, i32>& levels, i32 depth);

        static void SortByLevel(Array<CookItem>& items);

        static void SortGuids(Array<Guid>& guids);

        // The product instance for an item: mirrored path, SAME guid, stamped with the
        // builder's product type. MUTATES the cooked DB - main thread only (see Execute).
        [[nodiscard]] content::Instance* EnsureProduct(const CookItem& item);

        // Cook one item into its pre-created product and update its record. Runs on WORKER
        // threads: no DB mutation here - only reads + the product's own file writes.
        [[nodiscard]] bool CookItem_(CookItem& item);

        // The cooked-DB group mirroring the source instance's group path.
        [[nodiscard]] content::Group* MirrorGroup(content::Group& sourceGroup);

        content::ContentDatabase* m_sourceDb;
        content::ContentDatabase* m_cookedDb;
        BuilderRegistry* m_builders;
        vfs::IFileSystem* m_sources; // nullable: embedded-data projects have no source files
        vfs::IFileSystem* m_cache;   // nullable: no persistence (tests / one-shot cooks)
        JobSystem* m_jobs;           // nullable: serial execution

        CookDb m_db;
        HashMap<Guid, u64> m_recipeMemo;                   // per-Plan recipe cache
        HashMap<Guid, Array<CookFileMemo>> m_pendingMemos; // file memos gathered during Plan
        Mutex m_recordMutex;                               // record updates from worker threads
    };
}
