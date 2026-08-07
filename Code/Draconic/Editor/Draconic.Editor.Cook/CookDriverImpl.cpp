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

module draconic.editor.cook;

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.editor;

using namespace draconic::foundation;

namespace draconic::editor
{
    CookRecord* CookDb::Find(const Guid& source)
    {
        CookRecord* const* r = m_records.Find(source);
        return (r != nullptr) ? *r : nullptr;
    }

    CookRecord& CookDb::Upsert(const Guid& source)
    {
        if (CookRecord* existing = Find(source))
        {
            return *existing;
        }
        auto record = MakeUnique<CookRecord>(DefaultAllocator());
        record->source = source;
        CookRecord* raw = record.Get();
        m_storage.PushBack(Move(record));
        m_records.InsertOrAssign(source, raw);
        return *raw;
    }

    void CookDb::Remove(const Guid& source)
    {
        m_records.Remove(source);
        for (usize i = 0; i < m_storage.Size(); ++i)
        {
            if (m_storage[i]->source == source)
            {
                m_storage.RemoveAtSwap(i);
                return;
            }
        }
    }

    void CookDb::ForEach(const Function<void(const CookRecord&)>& fn) const
    {
        for (const UniquePtr<CookRecord>& r : m_storage)
        {
            fn(*r);
        }
    }

    void CookDb::Load(vfs::IFileSystem& cache, StringView name)
    {
        m_records.Clear();
        m_storage.Clear();
        UniquePtr<IStream> stream = cache.Open(name, FileMode::Read);
        if (stream.Get() == nullptr)
        {
            return;
        }

        BinarySerializer ar(*stream, SerializeMode::Read);
        u32 version = 0;
        u64 count = 0;
        Serialize(ar, "version", version);
        if (!ar.IsOk() || version != kVersion)
        {
            m_records.Clear();
            m_storage.Clear();
            return;
        }
        Serialize(ar, "count", count);
        for (u64 i = 0; ar.IsOk() && i < count; ++i)
        {
            CookRecord record;
            SerializeRecord(ar, record);
            if (!ar.IsOk())
            {
                break;
            }
            Upsert(record.source) = Move(record);
        }
        if (!ar.IsOk())
        {
            m_records.Clear();
            m_storage.Clear();
        } // corrupt -> full re-plan
    }

    Status CookDb::Save(vfs::IFileSystem& cache, StringView name) const
    {
        vfs::IWritableFileSystem* writable = cache.AsWritable();
        if (writable == nullptr)
        {
            return Status{ErrorCode::NotSupported};
        }

        MemoryStream buffer;
        BinarySerializer ar(buffer, SerializeMode::Write);
        u32 version = kVersion;
        u64 count = m_storage.Size();
        Serialize(ar, "version", version);
        Serialize(ar, "count", count);
        for (const UniquePtr<CookRecord>& r : m_storage)
        {
            SerializeRecord(ar, const_cast<CookRecord&>(*r));
        }
        if (!ar.IsOk())
        {
            return Status{ErrorCode::Internal};
        }
        return writable->Save(name, buffer.Bytes());
    }

    void CookDb::SerializeRecord(ISerializer& ar, CookRecord& r)
    {
        Serialize(ar, "source", r.source);
        Serialize(ar, "recipeHash", r.recipeHash);
        Serialize(ar, "failed", r.failed);
        Serialize(ar, "files", r.files);
        Serialize(ar, "reads", r.reads);
        Serialize(ar, "references", r.references);
    }
    CookPlan CookDriver::PlanFor(Span<const Guid> roots, bool force)
    {
        CookPlan plan;
        m_recipeMemo.Clear();

        Array<Guid> queue;
        Array<Guid> visited;
        for (const Guid& id : roots)
        {
            queue.PushBack(id);
        }
        HashMap<Guid, i32> levels;
        usize head = 0;
        while (head < queue.Size())
        {
            const Guid id = queue[head++];
            bool seen = false;
            for (const Guid& v : visited)
            {
                if (v == id)
                {
                    seen = true;
                    break;
                }
            }
            if (seen)
            {
                continue;
            }
            visited.PushBack(id);

            content::Instance* instance = m_sourceDb->GetInstance(id);
            if (instance == nullptr)
            {
                continue;
            }
            bool isRoot = false;
            for (const Guid& r : roots)
            {
                if (r == id)
                {
                    isRoot = true;
                    break;
                }
            }

            AssetDependencies deps;
            PlanInstance(*instance, force && isRoot, plan, levels, &deps);
            for (const Guid& dep : deps.reads)
            {
                queue.PushBack(dep);
            }
            for (const Guid& dep : deps.references)
            {
                queue.PushBack(dep);
            }
        }
        SortByLevel(plan.dirty);
        plan.reachable = Move(visited); // the full closure (roots + transitive deps)
        return plan;
    }

    CookPlan CookDriver::Plan(bool force)
    {
        CookPlan plan;
        m_recipeMemo.Clear();

        // Gather every buildable source instance.
        Array<content::Instance*> instances;
        CollectInstances(m_sourceDb->RootGroup(), instances);

        HashMap<Guid, i32> levels; // read-dep depth per source (0 = no reads)
        for (content::Instance* instance : instances)
        {
            PlanInstance(*instance, force, plan, levels, nullptr);
        }

        // Dependency order: stable sort by level (reads cook before their consumers).
        SortByLevel(plan.dirty);

        // Orphan sweep: records whose source instance no longer exists.
        Array<Guid> orphans;
        m_db.ForEach(
            [&](const CookRecord& record)
            {
                if (m_sourceDb->GetInstance(record.source) == nullptr)
                {
                    orphans.PushBack(record.source);
                }
            });
        plan.orphans = Move(orphans);
        return plan;
    }

    void CookDriver::PlanInstance(content::Instance& instanceRef, bool force, CookPlan& plan,
                                  HashMap<Guid, i32>& levels, AssetDependencies* outDeps)
    {
        content::Instance* instance = &instanceRef;
        IAssetBuilder* builder = m_builders->FindByTypeName(instance->TypeName());
        if (builder == nullptr)
        {
            ++plan.unbuildable;
            return;
        }

        CookItem item;
        item.source = instance->Id();
        item.path = instance->Path();
        item.builder = builder;
        item.asset = instance->ReadObject();
        if (item.asset.Get() == nullptr)
        {
            // Deserialization failed - usually a source written by an OLDER schema
            // (no asset compatibility by policy): delete + re-import it.
            DRACONIC_LOG_WARNING(u8"Cook",
                                 u8"'{}' failed to deserialize (stale schema? delete + re-import)",
                                 item.path);
            ++plan.unbuildable;
            return;
        }
        Asset* asset = Cast<Asset>(item.asset.Get());
        if (asset == nullptr)
        {
            DRACONIC_LOG_WARNING(u8"Cook", u8"'{}' has a builder but is not an Asset", item.path);
            ++plan.unbuildable;
            return;
        }

        AssetBuildContext scanCtx;
        scanCtx.sources = m_sources;
        scanCtx.db = m_sourceDb;
        builder->ScanDependencies(*asset, scanCtx, item.deps);
        if (outDeps != nullptr)
        {
            *outDeps = item.deps;
        }

        item.recipeHash = ComputeRecipe(*instance, *asset, *builder, item.deps, 0);
        item.level = ReadDepth(item.source, item.deps, levels, 0);

        const CookRecord* record = m_db.Find(item.source);
        const bool productExists = m_cookedDb->GetInstance(item.source) != nullptr;
        const bool clean = !force && record != nullptr && !record->failed &&
                           record->recipeHash == item.recipeHash && productExists;
        if (clean)
        {
            ++plan.upToDate;
        }
        else
        {
            plan.dirty.PushBack(Move(item));
        }
    }

    CookStats CookDriver::Execute(CookPlan& plan, const CookProgress* progress)
    {
        PrepareProducts(plan);
        return ExecuteBuilds(plan, progress);
    }

    void CookDriver::PrepareProducts(CookPlan& plan)
    {
        for (const Guid& orphan : plan.orphans)
        {
            if (m_cookedDb->GetInstance(orphan) != nullptr)
            {
                (void)m_cookedDb->DeleteInstance(orphan);
            }
            m_db.Remove(orphan);
            ++plan.orphansSweptCount;
        }

        for (CookItem& item : plan.dirty)
        {
            item.product = EnsureProduct(item);
            item.sourceInstance = m_sourceDb->GetInstance(item.source);
        }
    }

    CookStats CookDriver::ExecuteBuilds(CookPlan& plan, const CookProgress* progress)
    {
        CookStats stats;
        stats.orphansSwept = plan.orphansSweptCount;

        Array<bool> results;
        results.Resize(plan.dirty.Size());
        usize done = 0;
        usize begin = 0;
        while (begin < plan.dirty.Size())
        {
            // The half-open range of the current dependency level.
            usize end = begin;
            while (end < plan.dirty.Size() && plan.dirty[end].level == plan.dirty[begin].level)
            {
                ++end;
            }

            if (m_jobs != nullptr && end - begin > 1)
            {
                m_jobs->ParallelFor(static_cast<u32>(end - begin), [&, begin](u32 i)
                                    { results[begin + i] = CookItem_(plan.dirty[begin + i]); });
            }
            else
            {
                for (usize i = begin; i < end; ++i)
                {
                    results[i] = CookItem_(plan.dirty[i]);
                }
            }

            for (usize i = begin; i < end; ++i)
            {
                results[i] ? ++stats.cooked : ++stats.failed;
                if (results[i])
                {
                    stats.cookedProducts.PushBack(plan.dirty[i].source);
                }
                ++done;
                if (progress != nullptr && progress->onItem)
                {
                    progress->onItem(done, plan.dirty.Size(), plan.dirty[i].path.AsView(),
                                     results[i]);
                }
            }
            begin = end;
        }

        if (m_cache != nullptr)
        {
            const Status saved = m_db.Save(*m_cache);
            if (!saved.IsOk())
            {
                DRACONIC_LOG_WARNING(u8"Cook", u8"pipeline db save failed");
            }
        }
        return stats;
    }

    void CookDriver::CollectInstances(content::Group* group, Array<content::Instance*>& out)
    {
        if (group == nullptr)
        {
            return;
        }
        for (content::Instance* instance : group->Instances())
        {
            out.PushBack(instance);
        }
        for (content::Group* child : group->Groups())
        {
            CollectInstances(child, out);
        }
    }

    u64 CookDriver::HashSourceFile(StringView path, const CookRecord* previous,
                                   Array<CookFileMemo>& outMemos)
    {
        CookFileMemo memo;
        memo.path = String(path);

        vfs::FileStatInfo stat;
        const bool hasStat = m_sources != nullptr && m_sources->AsStat() != nullptr &&
                             m_sources->AsStat()->Stat(path, stat);
        if (hasStat && previous != nullptr)
        {
            for (const CookFileMemo& old : previous->files)
            {
                if (old.path == path && old.size == stat.size &&
                    old.modifiedTime == stat.modifiedTime)
                {
                    memo = old; // untouched since last cook: reuse the content hash
                    outMemos.PushBack(Move(memo));
                    return outMemos[outMemos.Size() - 1].contentHash;
                }
            }
        }

        u64 hash = 0; // missing file hashes as 0 (the recipe still changes when it appears)
        if (m_sources != nullptr)
        {
            UniquePtr<IStream> stream = m_sources->Open(path, FileMode::Read);
            if (stream.Get() != nullptr)
            {
                hash = detail::HashStream(*stream);
            }
        }
        memo.contentHash = hash;
        if (hasStat)
        {
            memo.size = stat.size;
            memo.modifiedTime = stat.modifiedTime;
        }
        outMemos.PushBack(Move(memo));
        return hash;
    }

    u64 CookDriver::ComputeRecipe(content::Instance& instance, const Asset& asset,
                                  IAssetBuilder& builder, const AssetDependencies& deps, i32 depth)
    {
        if (const u64* memo = m_recipeMemo.Find(instance.Id()))
        {
            return *memo;
        }
        if (depth > 64)
        {
            DRACONIC_LOG_WARNING(u8"Cook", u8"read-dependency cycle at '{}'", instance.Path());
            return 0;
        }

        u64 h = HashBytes(nullptr, 0);

        // 1. The source envelope (import settings + identity + embedded stream directory).
        {
            UniquePtr<IStream> envelope = instance.OpenEnvelope();
            const u64 envelopeHash =
                (envelope.Get() != nullptr) ? detail::HashStream(*envelope) : 0;
            h = detail::FoldHash(h, 'E', envelopeHash);
        }

        // 2. Source files: the implicit fileName + declared extras, in declaration order.
        const CookRecord* previous = m_db.Find(instance.Id());
        Array<CookFileMemo> memos;
        if (!asset.fileName.IsEmpty())
        {
            h = detail::FoldHash(h, 'F', HashSourceFile(asset.fileName.View(), previous, memos));
        }
        for (const draconic::vfs::SourcePath& file : deps.files)
        {
            h = detail::FoldHash(h, 'F', HashSourceFile(file.View(), previous, memos));
        }
        m_pendingMemos.InsertOrAssign(instance.Id(), Move(memos));

        // 2b. Declared embedded streams (sidecar files the envelope hash doesn't cover).
        for (const String& streamName : deps.sourceStreams)
        {
            UniquePtr<IStream> stream = instance.ReadData(streamName.AsView());
            h = detail::FoldHash(h, 'S',
                                 (stream.Get() != nullptr) ? detail::HashStream(*stream) : 0);
        }

        // 3. Builder version.
        h = detail::FoldHash(h, 'V', builder.Version());

        // 4. Read deps (sorted by Guid for determinism), chained recursively.
        Array<Guid> reads(deps.reads);
        SortGuids(reads);
        for (const Guid& read : reads)
        {
            u64 readRecipe = 0;
            if (content::Instance* dep = m_sourceDb->GetInstance(read))
            {
                if (IAssetBuilder* depBuilder = m_builders->FindByTypeName(dep->TypeName()))
                {
                    RefPtr<ISerializable> depObject = dep->ReadObject();
                    if (Asset* depAsset = Cast<Asset>(depObject.Get()))
                    {
                        AssetBuildContext scanCtx;
                        scanCtx.sources = m_sources;
                        scanCtx.db = m_sourceDb;
                        AssetDependencies depDeps;
                        depBuilder->ScanDependencies(*depAsset, scanCtx, depDeps);
                        readRecipe =
                            ComputeRecipe(*dep, *depAsset, *depBuilder, depDeps, depth + 1);
                    }
                }
            }
            h = detail::FoldHash(h, 'R', readRecipe);
            h = detail::FoldHash(h, 'G', read.high);
            h = detail::FoldHash(h, 'g', read.low);
        }

        m_recipeMemo.InsertOrAssign(instance.Id(), h);
        return h;
    }

    i32 CookDriver::ReadDepth(const Guid& source, const AssetDependencies& deps,
                              HashMap<Guid, i32>& levels, i32 depth)
    {
        if (const i32* known = levels.Find(source))
        {
            return *known;
        }
        if (depth > 64)
        {
            return depth;
        } // cycle guard (already warned in ComputeRecipe)
        i32 level = 0;
        for (const Guid& read : deps.reads)
        {
            content::Instance* dep = m_sourceDb->GetInstance(read);
            if (dep == nullptr)
            {
                continue;
            }
            IAssetBuilder* depBuilder = m_builders->FindByTypeName(dep->TypeName());
            if (depBuilder == nullptr)
            {
                continue;
            }
            RefPtr<ISerializable> depObject = dep->ReadObject();
            Asset* depAsset = Cast<Asset>(depObject.Get());
            if (depAsset == nullptr)
            {
                continue;
            }
            AssetBuildContext scanCtx;
            scanCtx.sources = m_sources;
            scanCtx.db = m_sourceDb;
            AssetDependencies depDeps;
            depBuilder->ScanDependencies(*depAsset, scanCtx, depDeps);
            const i32 depLevel = ReadDepth(read, depDeps, levels, depth + 1);
            if (depLevel + 1 > level)
            {
                level = depLevel + 1;
            }
        }
        levels.InsertOrAssign(source, level);
        return level;
    }

    void CookDriver::SortByLevel(Array<CookItem>& items)
    {
        // Insertion sort (stable; plans are small and mostly ordered).
        for (usize i = 1; i < items.Size(); ++i)
        {
            usize j = i;
            while (j > 0 && items[j - 1].level > items[j].level)
            {
                CookItem tmp = Move(items[j - 1]);
                items[j - 1] = Move(items[j]);
                items[j] = Move(tmp);
                --j;
            }
        }
    }

    void CookDriver::SortGuids(Array<Guid>& guids)
    {
        for (usize i = 1; i < guids.Size(); ++i)
        {
            usize j = i;
            auto less = [](const Guid& a, const Guid& b)
            { return a.high < b.high || (a.high == b.high && a.low < b.low); };
            while (j > 0 && less(guids[j], guids[j - 1]))
            {
                const Guid tmp = guids[j - 1];
                guids[j - 1] = guids[j];
                guids[j] = tmp;
                --j;
            }
        }
    }

    content::Instance* CookDriver::EnsureProduct(const CookItem& item)
    {
        const TypeInfo* productType =
            (item.builder != nullptr) ? item.builder->ProductType() : nullptr;
        if (productType == nullptr)
        {
            return nullptr;
        }
        const StringView typeName(reinterpret_cast<const utf8char*>(productType->name));

        if (content::Instance* existing = m_cookedDb->GetInstance(item.source))
        {
            // Identity guard: a guid collision across generations (e.g. a replayed guid
            // sequence) can leave this guid on a DIFFERENT product type. Writing this
            // item's product into it would cross-type the envelope - readers then
            // deserialize garbage. Recreate it with the right type instead.
            if (existing->TypeName() == typeName)
            {
                return existing;
            }
            (void)m_cookedDb->DeleteInstance(item.source);
        }
        content::Instance* source = m_sourceDb->GetInstance(item.source);
        if (source == nullptr)
        {
            return nullptr;
        }
        content::Group* group = MirrorGroup(source->OwningGroup());

        // Same guard for a NAME collision: CreateInstanceWithId returns an existing
        // same-named instance REGARDLESS of its id - a stale product from a previous
        // source generation (not yet swept) would silently keep its old guid, breaking
        // the product-guid == source-guid invariant. Remove it first.
        if (content::Instance* stale = group->GetInstance(source->Name()))
        {
            if (stale->Id() != item.source)
            {
                (void)m_cookedDb->DeleteInstance(stale->Id());
            }
        }
        return group->CreateInstanceWithId(item.source, source->Name(), *productType);
    }

    bool CookDriver::CookItem_(CookItem& item)
    {
        content::Instance* source = item.sourceInstance; // snapshotted (no DB query off-thread)
        Asset* asset = Cast<Asset>(item.asset.Get());
        content::Instance* product = item.product;
        if (source == nullptr || asset == nullptr || product == nullptr)
        {
            return false;
        }

        AssetBuildContext ctx;
        ctx.sources = m_sources;
        ctx.source = source;
        ctx.output = product;
        ctx.db = m_cookedDb; // cross-refs resolve against already-cooked products
        const Status built = item.builder->Build(*asset, ctx);

        // Record: recipe + memoized file hashes + deps; failures keep the last good
        // product but stay dirty (failed record never satisfies a clean check).
        ScopedLock lock(m_recordMutex);
        CookRecord& record = m_db.Upsert(item.source);
        record.recipeHash = item.recipeHash;
        record.failed = !built.IsOk();
        if (Array<CookFileMemo>* memos = m_pendingMemos.Find(item.source))
        {
            record.files = Move(*memos);
        }
        record.reads = item.deps.reads;
        record.references = item.deps.references;
        if (!built.IsOk())
        {
            DRACONIC_LOG_ERROR(u8"Cook", u8"'{}' failed to cook", item.path);
        }
        // Release the deserialized source NOW: plans previously kept every item's object
        // (full mesh vertex blobs, texture tables) alive until the whole cook finished -
        // large scenes (Sponza) ran the process out of memory.
        item.asset = RefPtr<ISerializable>{};
        return built.IsOk();
    }

    content::Group* CookDriver::MirrorGroup(content::Group& sourceGroup)
    {
        Array<StringView> chain;
        for (content::Group* g = &sourceGroup; g != nullptr && !g->Name().IsEmpty();
             g = g->Parent())
        {
            chain.PushBack(g->Name());
        }
        content::Group* group = m_cookedDb->RootGroup();
        for (usize i = chain.Size(); i > 0; --i)
        {
            group = group->CreateGroup(chain[i - 1]);
        }
        return group;
    }
}
