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

export module editor.project:cook_service;

import foundation.core;
import foundation.content;
import foundation.vfs;
import pipeline.core;
import pipeline.cook;
import :project;

using namespace foundation::core;
using namespace pipeline;

export namespace editor
{
    enum class CookBadge : u8
    {
        NoBuilder,
        Cooked,
        Missing,
        Failed
    };

    /// What the most recent cook did: the plan's split (planned, the dirty items; upToDate;
    /// unbuildable) and the build's counts. Valid from when OnCookFinished fires until the
    /// next cook finishes; the app's result toast and the MCP asset_cook read it.
    struct CookSummary
    {
        usize planned = 0;
        usize cooked = 0;
        usize failed = 0;
        usize orphansSwept = 0;
        usize upToDate = 0;
        usize unbuildable = 0;
    };

    class EditorCookService
    {
    public:
        /// Fired on the main thread (from Update) when a cook finishes.
        Function<void()> OnCookFinished;

        ~EditorCookService() { Shutdown(); }

        /// Wires the service to the open project. `builders` must outlive the service (the executable
        /// assembles the registry before the project opens).
        void Initialize(EditorProject& project, BuilderRegistry& builders);

        void Shutdown();

        [[nodiscard]] bool IsReady() const noexcept { return m_driver.Get() != nullptr; }
        [[nodiscard]] bool IsCooking() const noexcept { return m_cooking.load(); }
        /// The instance's cook recipe hash (the thumbnail content key), or 0 when
        /// unknown (mid-cook, when the DB belongs to the worker, or never cooked).
        [[nodiscard]] u64 RecipeHashFor(const Guid& id);

        /// An external contributor to the mutation lock, wired by the app. For example, a
        /// background export job reads the source DB structure and packs cooked files from its
        /// worker, so DB mutations and new cooks must hold off just as during a cook.
        Function<bool()> ExternalMutationLock;

        /// True while any background reader of the DBs is in flight (a cook, or an external job
        /// through ExternalMutationLock). Every structural-mutation gate and the watcher check
        /// this, not IsCooking alone.
        [[nodiscard]] bool MutationLocked() const;

        /// Fully quiescent: nothing in flight and no remembered request waiting to be issued
        /// again (a RequestCook that arrived mid-cook). Gates that start something after the cook
        /// (play in editor) check this rather than MutationLocked, because between a finishing
        /// cook and its remembered re-issue there is an idle gap that a MutationLocked poll would
        /// mistake for done.
        [[nodiscard]] bool IsIdle() const
        {
            return !MutationLocked() && !m_pendingCook && m_pendingRoots.IsEmpty();
        }

        /// Bumped when a cook finishes; the UI (badges) refreshes from it.
        [[nodiscard]] u64 Revision() const noexcept { return m_revision; }

        /// Starts a background cook. A request while one is running is remembered and issued
        /// again when it finishes, so a save during a cook does not lose its recook. `force`
        /// rebuilds everything.
        void RequestCook(bool force = false);

        /// Scoped cook: the given source instances plus their dependency closure (a group's
        /// ids, one asset, ...). The same three-phase flow as RequestCook; queued through
        /// RunWhenIdle when a cook is already in flight. No orphan sweep, which is a whole-project
        /// concern.
        void RequestCookFor(Array<Guid> roots, bool force = false);

        /// Hand-off to phases 2 and 3: PrepareProducts on the main thread, then the build worker.
        void StartBuilds();

        /// Main-thread pump: drains progress messages into `status` (e.g. the status bar and
        /// console) and fires OnCookFinished after a cook completes.
        void Update(const Function<void(StringView)>& status);

        /// Defers a main-thread source-DB mutation (import, delete) until no cook is in flight:
        /// the worker reads instance pointers snapshotted at plan time, so creating or deleting
        /// instances mid-cook is a race. Runs immediately when idle. Main thread only, like every
        /// other DB entry point.
        void RunWhenIdle(Function<void()> action);

        /// Products rebuilt by the most recent cook (valid after OnCookFinished fires, until
        /// the next cook finishes). The app hot-reloads these through the ResourceManager.
        [[nodiscard]] Span<const Guid> LastCookedProducts() const noexcept;

        /// The most recent cook's counts (valid when OnCookFinished fires, like
        /// LastCookedProducts).
        [[nodiscard]] const CookSummary& LastCookSummary() const noexcept
        {
            return m_lastSummaryMain;
        }

        /// Cheap per-instance cook state for the Assets panel (no recipe recompute).
        [[nodiscard]] CookBadge BadgeFor(foundation::content::Instance& instance);

    private:
        void Post(String message);

        void JoinWorker();

        static void AppendCount(String& out, usize value);

        [[nodiscard]] static String FormatPlanned(usize dirty, usize orphans);

        static constexpr f64 kWatchPollSeconds = 2.0;

        EditorProject* m_project = nullptr;    // borrowed
        BuilderRegistry* m_builders = nullptr; // borrowed (assembled by the executable)
        UniquePtr<foundation::vfs::NativeFileSystem> m_sources;
        UniquePtr<foundation::vfs::NativeFileSystem> m_cache;
        UniquePtr<JobSystem> m_jobs;
        UniquePtr<CookDriver> m_driver;
        UniquePtr<Thread> m_worker;

        Atomic<bool> m_cooking{false};
        Atomic<bool> m_finishedPending{false};
        u64 m_revision = 0;
        Mutex m_queueMutex;
        Array<String> m_queue;
        Array<Guid> m_lastCooked;     // written by the worker under m_queueMutex
        Array<Guid> m_lastCookedMain; // main-thread copy (LastCookedProducts)
        CookSummary m_lastSummary;     // the plan half written on the main thread at StartBuilds,
                                       // the build half by the worker, both under m_queueMutex
        CookSummary m_lastSummaryMain; // main-thread copy (LastCookSummary)
        Array<Function<void()>> m_idleQueue; // main-thread deferred mutations (RunWhenIdle)
        bool m_pendingCook = false;          // a RequestCook arrived while cooking
        bool m_pendingForce = false;
        Array<Guid> m_pendingRoots; // merged scoped requests that arrived mid-cook
        bool m_pendingRootsForce = false;
        CookPlan m_plan; // planned on the worker, prepared on the main thread, built on the worker
        Atomic<bool> m_planReady{false};
        foundation::vfs::IChangeSource* m_watcher = nullptr; // borrowed (sources mount owns it)
        Array<String> m_watchChanged;
        f64 m_lastWatchPoll = 0.0;
    };
}
