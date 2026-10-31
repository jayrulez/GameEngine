// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :cook_service partition.
//
// EditorCookService is the editor's front end to the cook driver. It owns the project's sources
// and .cache mounts and a CookDriver over the project DBs, and runs cooks on a background thread,
// one at a time, so the UI stays live; progress messages queue through a mutex and drain on the
// main thread in Update(). Cook badges give the Assets panel a cheap per-instance state without
// recomputing recipes (exact dirtiness is the driver's job at cook time):
//   NoBuilder: the instance's type has no registered builder (scenes, raw data)
//   Failed:    the last cook of this asset failed (the Console has the log)
//   Missing:   no record or no product yet (never cooked, or swept)
//   Cooked:    record and product exist (a stale recipe still shows Cooked until the next cook)
//
// The service also watches the project's Sources/ tree (the native mount's stat-sweep
// IChangeSource, polled every couple of seconds from Update): an external edit of a source file
// queues an automatic incremental cook, and the driver's plan recomputes exactly what the change
// dirtied. After every cook, LastCookedProducts() lists the rebuilt product guids so the
// application can hot-reload them through the ResourceManager (a proxy swap in live scenes).
//
// Known hazard: editing source assets while a cook runs races the driver's source reads. The UI
// disables cook triggers during a cook but does not lock edits.

module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

module editor.project;

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.cook;
import :project;

using namespace foundation::core;
using namespace pipeline;

namespace editor
{
    void EditorCookService::Initialize(EditorProject& project, BuilderRegistry& builders)
    {
        Shutdown();
        m_project = &project;
        m_builders = &builders;
        m_sources = MakeUnique<foundation::vfs::NativeFileSystem>(
            editor::EditorRootAllocator(), project.SourcesRoot().AsView(), editor::EditorRootAllocator());
        m_cache = MakeUnique<foundation::vfs::NativeFileSystem>(
            editor::EditorRootAllocator(), project.CacheRoot().AsView(), editor::EditorRootAllocator());
        m_jobs = MakeUnique<JobSystem>(editor::EditorRootAllocator(), editor::EditorRootAllocator());
        m_driver = MakeUnique<CookDriver>(editor::EditorRootAllocator(),
                                          editor::EditorRootAllocator(), project.SourceDb(),
                                          project.CookedDb(), builders, m_sources.Get(),
                                          m_cache.Get(), m_jobs.Get());

        // Watch Sources/ for external edits (stat sweep; throttled from Update).
        if (foundation::vfs::IWatchableFileSystem* watchable = m_sources->AsWatchable())
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
        // Reset to quiescent, whatever phase the join cut short: a worker joined mid-plan never
        // reaches the build that clears m_cooking, and a stuck flag would leave the next project's
        // first request "remembered" forever. The deferred mutations belonged to the closed project.
        m_cooking.store(false);
        m_planReady.store(false);
        m_finishedPending.store(false);
        m_pendingCook = false;
        m_pendingForce = false;
        m_pendingRoots.Clear();
        m_pendingRootsForce = false;
        m_idleQueue.Clear();
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
        if (MutationLocked()) // a cook is running or an external job is reading the DBs
        {
            m_pendingCook = true;
            m_pendingForce = m_pendingForce || force;
            return;
        }
        JoinWorker(); // reap the previous worker's handle

        // Three phases, so the UI never stalls and nothing races:
        //   1. Plan on the worker. It only reads the DBs, and their structure is frozen while
        //      IsCooking(): every main-thread structural mutation (import, delete, rename,
        //      create) goes through RunWhenIdle. Planning hashes every source file, which would
        //      freeze the editor for whole rebuilds on the main thread.
        //   2. PrepareProducts on the main thread (Update drains m_planReady). This is the only
        //      phase that changes the cooked DB, which main-thread resource loads read at any
        //      time.
        //   3. Builds on the worker (no DB queries; it uses snapshotted pointers).
        m_cooking.store(true);
        CookDriver* driver = m_driver.Get();
        EditorCookService* self = this;
        m_worker = MakeUnique<Thread>(editor::EditorRootAllocator(),
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
            // Merge rather than queue closures: concurrent requests for overlapping roots (several
            // pages opening over the same uncooked assets) replay as one scoped cook. A pending
            // full cook supersedes them, since its plan covers any roots.
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
        m_worker = MakeUnique<Thread>(editor::EditorRootAllocator(),
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
        {
            ScopedLock lock(m_queueMutex);
            m_lastSummary = CookSummary{};
            m_lastSummary.planned = total;
            m_lastSummary.upToDate = m_plan.upToDate;
            m_lastSummary.unbuildable = m_plan.unbuildable;
        }
        // Plans with no work run silently (no "cooking 0 asset(s)" line): page-open and import
        // requests are cheap to make and often find everything already cooked.
        if (total > 0 || !m_plan.orphans.IsEmpty())
        {
            Post(FormatPlanned(total, m_plan.orphans.Size()));
        }

        CookDriver* driver = m_driver.Get();
        EditorCookService* self = this;
        m_worker = MakeUnique<Thread>(editor::EditorRootAllocator(),
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
                                              self->m_lastSummary.cooked = stats.cooked;
                                              self->m_lastSummary.failed = stats.failed;
                                              self->m_lastSummary.orphansSwept = stats.orphansSwept;
                                          }
                                          self->m_cooking.store(false);
                                          self->m_finishedPending.store(true);
                                      });
    }

    void EditorCookService::Update(const Function<void(StringView)>& status)
    {
        // Phase hand-off: the plan worker finished, so pre-create the products on the main thread
        // and start the build worker.
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
            LOG_INFO(u8"Cook", u8"{}", line);
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
                m_lastSummaryMain = m_lastSummary;
            }
            if (OnCookFinished)
            {
                OnCookFinished();
            }
        }

        // Deferred source-DB mutations (imports and deletes queued while the cook worker was
        // reading snapshotted instances), and a cook request that arrived mid-cook.
        if (!MutationLocked())
        {
            if (!m_idleQueue.IsEmpty())
            {
                Array<Function<void()>> pending = Move(m_idleQueue);
                m_idleQueue = Array<Function<void()>>{};
                for (Function<void()>& action : pending)
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

        // Watcher: a throttled stat sweep, where any source change queues an incremental cook.
        // Held off while any background DB reader runs (a cook during an export would rewrite
        // the cooked files the export job is packing).
        if (m_watcher != nullptr && !MutationLocked())
        {
            const f64 now = TicksToSeconds(GetTicks());
            if (now - m_lastWatchPoll >= kWatchPollSeconds)
            {
                m_lastWatchPoll = now;
                m_watchChanged.Clear();
                if (m_watcher->Poll(m_watchChanged))
                {
                    LOG_INFO(u8"Cook", u8"{} source file(s) changed - recooking",
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

    u64 EditorCookService::RecipeHashFor(const Guid& id)
    {
        if (!IsReady() || IsCooking())
        {
            return 0; // the db belongs to the worker mid-cook
        }
        const CookRecord* record = m_driver->Db().Find(id);
        return record != nullptr && !record->failed ? record->recipeHash : 0;
    }

    CookBadge EditorCookService::BadgeFor(foundation::content::Instance& instance)
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
