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

export module draconic.editor.core:cook_service;

import draconic.foundation;
import draconic.content;
import draconic.vfs;
import draconic.editor;
import draconic.editor.cook;
import :project;

using namespace draconic::foundation;

export namespace draconic::editor
{
    enum class CookBadge : u8
    {
        NoBuilder,
        Cooked,
        Missing,
        Failed
    };

    class EditorCookService
    {
    public:
        /// Fired on the main thread (from Update) when a cook finishes.
        Function<void()> OnCookFinished;

        ~EditorCookService() { Shutdown(); }

        /// Wire to the open project. `builders` must outlive the service (the executable
        /// assembles the registry before the project opens).
        void Initialize(EditorProject& project, BuilderRegistry& builders);

        void Shutdown();

        [[nodiscard]] bool IsReady() const noexcept { return m_driver.Get() != nullptr; }
        [[nodiscard]] bool IsCooking() const noexcept { return m_cooking.load(); }

        /// External contributor to the mutation lock (wired by the app): e.g. a background
        /// EXPORT job reads the source DB structure and packs cooked files from its worker,
        /// so DB mutations and new cooks must hold off exactly like during a cook.
        Function<bool()> ExternalMutationLock;

        /// True while ANY background reader of the DBs is in flight (a cook, or an external
        /// job via ExternalMutationLock). Every structural-mutation gate and the watcher key
        /// on THIS, not on IsCooking alone.
        [[nodiscard]] bool MutationLocked() const;

        /// Bumped when a cook finishes - UI (badges) refreshes off it.
        [[nodiscard]] u64 Revision() const noexcept { return m_revision; }

        /// Kick a background cook. A request while one is running is REMEMBERED and re-
        /// issued when it finishes (previously it was silently dropped - a save-during-cook
        /// lost its recook). `force` = rebuild all.
        void RequestCook(bool force = false);

        /// Scoped cook: the given source instances plus their dependency closure (a group's
        /// ids, one asset, ...). Same three-phase flow as RequestCook; queued via RunWhenIdle
        /// when a cook is already in flight. No orphan sweep (whole-project concern).
        void RequestCookFor(Array<Guid> roots, bool force = false);

        /// Phase 2+3 hand-off: PrepareProducts on the main thread, then the build worker.
        void StartBuilds();

        /// Main-thread pump: drains progress messages into `status` (e.g. the status bar +
        /// console) and fires OnCookFinished after a cook completes.
        void Update(const Function<void(StringView)>& status);

        /// Defer a MAIN-THREAD source-DB mutation (import/delete) until no cook is in
        /// flight: the worker reads instance pointers snapshotted at plan time, so creating
        /// or deleting instances mid-cook is a race. Runs immediately when idle. Main-thread
        /// only (like every other DB entry point).
        void RunWhenIdle(Function<void()> action);

        /// Products rebuilt by the most recent cook (valid after OnCookFinished fires, until
        /// the next cook finishes). The app hot-reloads these through the ResourceManager.
        [[nodiscard]] Span<const Guid> LastCookedProducts() const noexcept;

        /// Pass/fail counts of the most recent cook (valid when OnCookFinished fires, like
        /// LastCookedProducts).
        [[nodiscard]] usize LastCookedCount() const noexcept { return m_lastCookedCountMain; }
        [[nodiscard]] usize LastFailedCount() const noexcept { return m_lastFailedCountMain; }

        /// Cheap per-instance cook state for the Assets panel (no recipe recompute).
        [[nodiscard]] CookBadge BadgeFor(draconic::content::Instance& instance);

    private:
        void Post(String message);

        void JoinWorker();

        static void AppendCount(String& out, usize value);

        [[nodiscard]] static String FormatPlanned(usize dirty, usize orphans);

        static constexpr f64 kWatchPollSeconds = 2.0;

        EditorProject* m_project = nullptr;    // borrowed
        BuilderRegistry* m_builders = nullptr; // borrowed (exe-assembled)
        UniquePtr<draconic::vfs::NativeFileSystem> m_sources;
        UniquePtr<draconic::vfs::NativeFileSystem> m_cache;
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
        usize m_lastCookedCount = 0;  // worker-written, under m_queueMutex
        usize m_lastFailedCount = 0;
        usize m_lastCookedCountMain = 0; // main-thread copies
        usize m_lastFailedCountMain = 0;
        Array<Function<void()>> m_idleQueue; // main-thread deferred mutations (RunWhenIdle)
        bool m_pendingCook = false;          // a RequestCook arrived while cooking
        bool m_pendingForce = false;
        Array<Guid> m_pendingRoots; // merged scoped requests that arrived mid-cook
        bool m_pendingRootsForce = false;
        CookPlan m_plan; // worker-planned, main-prepared, worker-built
        Atomic<bool> m_planReady{false};
        draconic::vfs::IChangeSource* m_watcher = nullptr; // borrowed (sources mount owns it)
        Array<String> m_watchChanged;
        f64 m_lastWatchPoll = 0.0;
    };
}
