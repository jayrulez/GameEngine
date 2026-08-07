// Draconic::EditorCore - :cook_service partition.
//
// EditorCookService: the in-editor face of the cook driver (asset-pipeline design §6). Owns the
// project's sources/.cache mounts + a CookDriver over the project DBs, and runs cooks on a
// BACKGROUND thread (one at a time - Traktor's build lock): the UI stays live, progress
// messages queue through a mutex and drain on the main thread via Update(). Cook badges give
// the Assets panel a cheap per-instance state without recomputing recipes (exact dirtiness is
// the driver's job at cook time):
//   NoBuilder - the instance's type has no registered builder (scenes, raw data)
//   Failed    - the last cook of this asset failed (Console has the log)
//   Missing   - no record or no product yet (never cooked, or swept)
//   Cooked    - record + product exist (a stale recipe still shows Cooked until the next cook)
//
// The service also watches the project's Sources/ tree (the native mount's stat-sweep
// IChangeSource, polled every couple of seconds from Update): an external edit of a source
// file queues an automatic incremental cook - the driver's plan recomputes exactly what the
// change dirtied. After every cook, LastCookedProducts() lists the rebuilt product guids so the
// application can hot-reload them through the ResourceManager (proxy swap in live scenes).
//
// Known v1 hazard (documented, Traktor shares it): editing source assets WHILE a cook runs
// races the driver's source reads; the UI disables cook triggers during a cook but does not
// lock edits.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Log/Log.h"

module draconic.editor.core;

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.editor;
import draconic.editor.cook;
import :project;

using namespace draconic::foundation;

namespace draconic::editor
{
    void EditorCookService::Initialize(EditorProject& project, BuilderRegistry& builders)
    {
        Shutdown();
        m_project = &project;
        m_builders = &builders;
        m_sources = MakeUnique<draconic::vfs::NativeFileSystem>(DefaultAllocator(),
                                                                project.SourcesRoot().AsView());
        m_cache = MakeUnique<draconic::vfs::NativeFileSystem>(DefaultAllocator(),
                                                              project.CacheRoot().AsView());
        m_jobs = MakeUnique<JobSystem>(DefaultAllocator());
        m_driver =
            MakeUnique<CookDriver>(DefaultAllocator(), project.SourceDb(), project.CookedDb(),
                                   builders, m_sources.Get(), m_cache.Get(), m_jobs.Get());

        // Watch Sources/ for external edits (stat sweep; throttled from Update).
        if (draconic::vfs::IWatchableFileSystem* watchable = m_sources->AsWatchable())
        {
            m_watcher = watchable->ChangeSource();
            if (m_watcher != nullptr)
            {
                m_watcher->Track(u8"");
            }
        }
        m_lastWatchPoll = TicksToSeconds(GetTicks());
    }

    void EditorCookService::Shutdown()
    {
        JoinWorker();
        m_watcher = nullptr; // owned by the sources mount
        m_driver.Reset();
        m_jobs.Reset();
        m_cache.Reset();
        m_sources.Reset();
        m_project = nullptr;
    }

    bool EditorCookService::MutationLocked() const
    {
        return IsCooking() || (ExternalMutationLock && ExternalMutationLock());
    }

    void EditorCookService::RequestCook(bool force)
    {
        if (!IsReady())
        {
            return;
        }
        if (MutationLocked()) // cook running OR an external job is reading the DBs
        {
            m_pendingCook = true;
            m_pendingForce = m_pendingForce || force;
            return;
        }
        JoinWorker(); // reap the previous worker's handle

        // Three phases so the UI never stalls AND nothing races:
        //   1. Plan on the WORKER - it only READS the DBs, and their structure is frozen
        //      while IsCooking(): every main-thread structural mutation (import, delete,
        //      rename, create) gates through RunWhenIdle. (Planning hashes every source
        //      file - on the main thread it froze the editor for whole rebuilds.)
        //   2. PrepareProducts on the MAIN thread (Update drains m_planReady) - the only
        //      phase that MUTATES the cooked DB, which main-thread resource loads read at
        //      any time.
        //   3. Builds on the worker (no DB queries - snapshotted pointers).
        m_cooking.store(true);
        CookDriver* driver = m_driver.Get();
        EditorCookService* self = this;
        m_worker = MakeUnique<Thread>(DefaultAllocator(),
                                      [self, driver, force]()
                                      {
                                          self->m_plan = driver->Plan(force);
                                          self->m_planReady.store(true);
                                      });
    }

    void EditorCookService::RequestCookFor(Array<Guid> roots, bool force)
    {
        if (!IsReady() || roots.IsEmpty())
        {
            return;
        }
        if (MutationLocked())
        {
            // MERGE, don't queue closures: concurrent requests for overlapping roots
            // (several pages opening over the same uncooked assets) replay as ONE
            // scoped cook. A pending FULL cook supersedes - its plan covers any roots.
            if (!m_pendingCook)
            {
                for (const Guid& id : roots)
                {
                    bool seen = false;
                    for (const Guid& existing : m_pendingRoots)
                    {
                        if (existing == id)
                        {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                    {
                        m_pendingRoots.PushBack(id);
                    }
                }
                m_pendingRootsForce = m_pendingRootsForce || force;
            }
            return;
        }
        JoinWorker();
        m_cooking.store(true);
        CookDriver* driver = m_driver.Get();
        EditorCookService* self = this;
        m_worker = MakeUnique<Thread>(DefaultAllocator(),
                                      [self, driver, roots = Move(roots), force]()
                                      {
                                          self->m_plan = driver->PlanFor(
                                              Span<const Guid>{roots.Data(), roots.Size()}, force);
                                          self->m_planReady.store(true);
                                      });
    }

    void EditorCookService::StartBuilds()
    {
        JoinWorker(); // the plan worker has finished (m_planReady was set)
        m_driver->PrepareProducts(m_plan);
        const usize total = m_plan.dirty.Size();
        // Zero-work plans run SILENTLY (no "cooking 0 asset(s)" line): page-open and
        // import requests are cheap to make and often find everything already cooked.
        if (total > 0 || !m_plan.orphans.IsEmpty())
        {
            Post(FormatPlanned(total, m_plan.orphans.Size()));
        }

        CookDriver* driver = m_driver.Get();
        EditorCookService* self = this;
        m_worker = MakeUnique<Thread>(DefaultAllocator(),
                                      [self, driver, total]()
                                      {
                                          CookPlan& plan = self->m_plan;
                                          CookProgress progress;
                                          progress.onItem = [self, total](usize done, usize,
                                                                          StringView path, bool ok)
                                          {
                                              String line(ok ? u8"cooked " : u8"FAILED ");
                                              line.Append(path);
                                              line.Append(u8" (");
                                              AppendCount(line, done);
                                              line.PushBack(utf8char('/'));
                                              AppendCount(line, total);
                                              line.PushBack(utf8char(')'));
                                              self->Post(Move(line));
                                          };
                                          CookStats stats = driver->ExecuteBuilds(plan, &progress);
                                          if (stats.cooked + stats.failed + stats.orphansSwept > 0)
                                          {
                                              String done(u8"cook finished: ");
                                              AppendCount(done, stats.cooked);
                                              done.Append(u8" cooked, ");
                                              AppendCount(done, stats.failed);
                                              done.Append(u8" failed");
                                              self->Post(Move(done));
                                          }
                                          {
                                              ScopedLock lock(self->m_queueMutex);
                                              self->m_lastCooked = Move(stats.cookedProducts);
                                              self->m_lastCookedCount = stats.cooked;
                                              self->m_lastFailedCount = stats.failed;
                                          }
                                          self->m_cooking.store(false);
                                          self->m_finishedPending.store(true);
                                      });
    }

    void EditorCookService::Update(const Function<void(StringView)>& status)
    {
        // Phase hand-off: the plan worker finished - run the main-thread product
        // pre-create and kick the build worker.
        if (m_planReady.exchange(false))
        {
            StartBuilds();
        }

        Array<String> drained;
        {
            ScopedLock lock(m_queueMutex);
            for (String& s : m_queue)
            {
                drained.PushBack(Move(s));
            }
            m_queue.Clear();
        }
        for (const String& line : drained)
        {
            DRACONIC_LOG_INFO(u8"Cook", u8"{}", line);
            if (status)
            {
                status(line.AsView());
            }
        }
        if (m_finishedPending.exchange(false))
        {
            ++m_revision;
            {
                ScopedLock lock(m_queueMutex);
                m_lastCookedMain = Move(m_lastCooked);
                m_lastCookedCountMain = m_lastCookedCount;
                m_lastFailedCountMain = m_lastFailedCount;
            }
            if (OnCookFinished)
            {
                OnCookFinished();
            }
        }

        // Deferred source-DB mutations (imports/deletes queued while the cook worker was
        // reading snapshotted instances) + a cook request that arrived mid-cook.
        if (!MutationLocked())
        {
            if (!m_idleQueue.IsEmpty())
            {
                Array<Function<void()>> drained = Move(m_idleQueue);
                m_idleQueue = Array<Function<void()>>{};
                for (Function<void()>& action : drained)
                {
                    action();
                }
            }
            if (m_pendingCook)
            {
                m_pendingCook = false;
                const bool force = m_pendingForce;
                m_pendingForce = false;
                m_pendingRoots.Clear(); // the full plan covers any scoped roots
                m_pendingRootsForce = false;
                RequestCook(force);
            }
            else if (!m_pendingRoots.IsEmpty())
            {
                Array<Guid> roots = Move(m_pendingRoots);
                m_pendingRoots = Array<Guid>{};
                const bool force = m_pendingRootsForce;
                m_pendingRootsForce = false;
                RequestCookFor(Move(roots), force);
            }
        }

        // Watcher: throttled stat sweep; any source change queues an incremental cook.
        // Held off while ANY background DB reader runs (a mid-export cook would rewrite
        // the very cooked files the export job is packing).
        if (m_watcher != nullptr && !MutationLocked())
        {
            const f64 now = TicksToSeconds(GetTicks());
            if (now - m_lastWatchPoll >= kWatchPollSeconds)
            {
                m_lastWatchPoll = now;
                m_watchChanged.Clear();
                if (m_watcher->Poll(m_watchChanged))
                {
                    DRACONIC_LOG_INFO(u8"Cook", u8"{} source file(s) changed - recooking",
                                      m_watchChanged.Size());
                    RequestCook(false);
                }
            }
        }
    }

    void EditorCookService::RunWhenIdle(Function<void()> action)
    {
        if (!MutationLocked())
        {
            if (action)
            {
                action();
            }
            return;
        }
        m_idleQueue.PushBack(Move(action));
    }

    Span<const Guid> EditorCookService::LastCookedProducts() const noexcept
    {
        return Span<const Guid>(m_lastCookedMain.Data(), m_lastCookedMain.Size());
    }

    CookBadge EditorCookService::BadgeFor(draconic::content::Instance& instance)
    {
        if (!IsReady() || m_builders == nullptr)
        {
            return CookBadge::NoBuilder;
        }
        if (m_builders->FindByTypeName(instance.TypeName()) == nullptr)
        {
            return CookBadge::NoBuilder;
        }
        if (IsCooking())
        {
            return CookBadge::Missing;
        } // db is the worker's during a cook
        const CookRecord* record = m_driver->Db().Find(instance.Id());
        if (record != nullptr && record->failed)
        {
            return CookBadge::Failed;
        }
        const bool productExists = m_project->CookedDb().GetInstance(instance.Id()) != nullptr;
        return (record != nullptr && productExists) ? CookBadge::Cooked : CookBadge::Missing;
    }

    void EditorCookService::Post(String message)
    {
        ScopedLock lock(m_queueMutex);
        m_queue.PushBack(Move(message));
    }

    void EditorCookService::JoinWorker()
    {
        if (m_worker)
        {
            m_worker->Join();
            m_worker.Reset();
        }
    }

    void EditorCookService::AppendCount(String& out, usize value)
    {
        utf8char digits[24];
        i32 n = 0;
        usize v = value;
        do
        {
            digits[n++] = static_cast<utf8char>('0' + v % 10);
            v /= 10;
        } while (v > 0 && n < 24);
        while (n > 0)
        {
            out.PushBack(digits[--n]);
        }
    }

    String EditorCookService::FormatPlanned(usize dirty, usize orphans)
    {
        String s(u8"cooking ");
        AppendCount(s, dirty);
        s.Append(u8" asset(s)");
        if (orphans > 0)
        {
            s.Append(u8", sweeping ");
            AppendCount(s, orphans);
            s.Append(u8" orphan(s)");
        }
        return s;
    }
}
