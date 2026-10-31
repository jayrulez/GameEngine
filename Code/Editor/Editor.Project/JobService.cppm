// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :job_service partition.
//
// A generic background-job runner with progress and step reporting for the editor domain's hosts,
// a generalized form of what EditorCookService does by hand. A submitted unit of work runs on a
// worker thread so the host's main loop stays live, reports progress and steps through a
// JobContext, and its completion fires on the main thread from Update(). Jobs run one at a time
// (a submit while busy queues), so a cook, export or import never races the DBs. The host pumps
// Update() each frame and reads Progress() for whatever it shows (the editor application's status
// bar; a tool's log line).

module;
#include "Core/Prelude.h"

export module editor.project:job_service;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    // Handed to a job's worker function for thread-safe progress reporting back to the host. One
    // mutex guards the whole snapshot (fraction, step, log); cancellation is a lock-free flag.
    class JobContext
    {
    public:
        // Overall progress, clamped to 0..1.
        void SetFraction(f32 fraction)
        {
            ScopedLock lock(m_mutex);
            m_fraction = (fraction < 0.0f) ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
        }

        // A named step, "index/count: label" (index is 1-based for display; 0/0 means no step breakdown).
        void SetStep(StringView label, usize index = 0, usize count = 0)
        {
            ScopedLock lock(m_mutex);
            m_step = String(label);
            m_stepIndex = index;
            m_stepCount = count;
        }

        // Queues a line for the app's log or console (drained on the main thread by Update).
        void Log(StringView message)
        {
            ScopedLock lock(m_mutex);
            m_log.PushBack(String(message));
        }

        // Cooperative cancellation: a long worker polls this and stops early if it is set.
        [[nodiscard]] bool CancelRequested() const noexcept { return m_cancel.load(); }

    private:
        friend class EditorJobService;

        mutable Mutex m_mutex;
        f32 m_fraction = 0.0f;
        String m_step;
        usize m_stepIndex = 0;
        usize m_stepCount = 0;
        Array<String> m_log;
        Atomic<bool> m_cancel{false};
    };

    class EditorJobService
    {
    public:
        // The allocator (required; the host decides) backs the worker threads and jobs.
        explicit EditorJobService(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        ~EditorJobService() { Shutdown(); }

        // Submits a job. `work` runs on a worker thread (reporting through its JobContext) and
        // returns a Status; `onDone` fires on the main thread (from Update) with that Status. Runs
        // immediately when idle, else queues behind the running job.
        void Submit(StringView title, Function<Status(JobContext&)> work,
                    Function<void(Status)> onDone = {})
        {
            m_queue.PushBack(Pending{String(title),
                                     static_cast<Function<Status(JobContext&)>&&>(work),
                                     static_cast<Function<void(Status)>&&>(onDone)});
            if (!m_running.load())
            {
                StartNext();
            }
        }

        [[nodiscard]] bool IsBusy() const noexcept
        {
            return m_running.load() || !m_queue.IsEmpty();
        }

        // --- light lane ---------------------------------------------------------------
        // Short CPU-only side work (editor preview bakes) on its own worker, concurrent with the
        // build lane. It stays outside IsBusy(), because that flag gates cook mutations and a
        // preview must never lock the build. There is no progress reporting and no cancellation,
        // since light work finishes in fractions of a second. `work` runs on the light worker;
        // `onDone` fires on the main thread from Update(). Results travel through state captured
        // by both closures (work writes before onDone reads; the completion flag publishes the
        // writes).
        void SubmitLight(Function<void()> work, Function<void()> onDone = {})
        {
            m_lightQueue.PushBack(PendingLight{static_cast<Function<void()>&&>(work),
                                               static_cast<Function<void()>&&>(onDone)});
            StartNextLight();
        }

        [[nodiscard]] bool IsLightBusy() const noexcept
        {
            return m_lightRunning.load() || !m_lightQueue.IsEmpty();
        }

        // Asks the running job to stop (cooperative; the worker must poll CancelRequested()).
        void CancelActive()
        {
            if (m_ctx)
            {
                m_ctx->m_cancel.store(true);
            }
        }

        // Main-thread pump: drains the active job's log to `log`, and on completion fires its
        // onDone and starts the next queued job. Call once per frame.
        void Update(const Function<void(StringView)>& log = {})
        {
            if (m_ctx)
            {
                Array<String> drained;
                {
                    ScopedLock lock(m_ctx->m_mutex);
                    drained = static_cast<Array<String>&&>(m_ctx->m_log);
                    m_ctx->m_log = Array<String>{};
                }
                if (log)
                {
                    for (const String& line : drained)
                    {
                        log(line.AsView());
                    }
                }
            }

            if (m_lightFinished.exchange(false))
            {
                JoinLightWorker();
                Function<void()> lightOnDone = static_cast<Function<void()>&&>(m_lightOnDone);
                m_lightRunning.store(false);
                if (lightOnDone)
                {
                    lightOnDone();
                } // may SubmitLight() another (queued)
                StartNextLight();
            }

            if (m_finished.exchange(false))
            {
                JoinWorker();
                // Final drain (the worker is joined now, so nothing more is appended): flush any
                // log lines the worker wrote after the drain above but before it finished.
                // Without this, m_ctx.Reset() below would discard a fast job's last log lines.
                if (m_ctx)
                {
                    Array<String> tail = static_cast<Array<String>&&>(m_ctx->m_log);
                    m_ctx->m_log = Array<String>{};
                    if (log)
                    {
                        for (const String& line : tail)
                        {
                            log(line.AsView());
                        }
                    }
                }
                Function<void(Status)> onDone = static_cast<Function<void(Status)>&&>(m_onDone);
                const Status result = m_result;
                m_ctx.Reset();
                m_running.store(false);
                if (onDone)
                {
                    onDone(result);
                } // may Submit() another job (queued)
                StartNext();
            }
        }

        // Snapshot of the running job for whoever shows it (active=false when idle). Main thread only.
        struct ProgressView
        {
            bool active = false;
            String title;
            f32 fraction = 0.0f;
            String step;
            usize stepIndex = 0;
            usize stepCount = 0;
        };

        [[nodiscard]] ProgressView Progress() const
        {
            ProgressView view;
            if (!m_running.load() || !m_ctx)
            {
                return view;
            }
            view.active = true;
            view.title = m_title; // set at StartNext (main thread), never touched by the worker
            ScopedLock lock(m_ctx->m_mutex);
            view.fraction = m_ctx->m_fraction;
            view.step = m_ctx->m_step;
            view.stepIndex = m_ctx->m_stepIndex;
            view.stepCount = m_ctx->m_stepCount;
            return view;
        }

        void Shutdown()
        {
            CancelActive(); // cooperative: a polling job stops early instead of blocking exit
            JoinWorker();
            JoinLightWorker();
        }

    private:
        struct Pending
        {
            String title;
            Function<Status(JobContext&)> work;
            Function<void(Status)> onDone;
        };

        struct PendingLight
        {
            Function<void()> work;
            Function<void()> onDone;
        };

        void JoinLightWorker()
        {
            if (m_lightWorker)
            {
                m_lightWorker->Join();
                m_lightWorker.Reset();
            }
        }

        // Main thread: dequeues and launches the next light job (does nothing if busy or empty).
        void StartNextLight()
        {
            if (m_lightRunning.load() || m_lightQueue.IsEmpty())
            {
                return;
            }
            PendingLight job = static_cast<PendingLight&&>(m_lightQueue[0]);
            m_lightQueue.RemoveAt(0);

            m_lightOnDone = static_cast<Function<void()>&&>(job.onDone);
            m_lightRunning.store(true);
            m_lightFinished.store(false);

            EditorJobService* self = this;
            m_lightWorker = MakeUnique<Thread>(
                (*m_allocator),
                [self, work = static_cast<Function<void()>&&>(job.work)]()
                {
                    if (work)
                    {
                        work();
                    }
                    self->m_lightFinished.store(true); // publishes the work's writes
                });
        }

        void JoinWorker()
        {
            if (m_worker)
            {
                m_worker->Join();
                m_worker.Reset();
            }
        }

        // Main thread: dequeues and launches the next job (does nothing if busy or empty).
        void StartNext()
        {
            if (m_running.load() || m_queue.IsEmpty())
            {
                return;
            }
            Pending job = static_cast<Pending&&>(m_queue[0]);
            m_queue.RemoveAt(0);

            m_title = static_cast<String&&>(job.title);
            m_onDone = static_cast<Function<void(Status)>&&>(job.onDone);
            m_ctx = MakeUnique<JobContext>((*m_allocator));
            m_result = Status{};
            m_running.store(true);
            m_finished.store(false);

            EditorJobService* self = this;
            JobContext* ctx = m_ctx.Get();
            Function<Status(JobContext&)> work =
                static_cast<Function<Status(JobContext&)>&&>(job.work);
            m_worker = MakeUnique<Thread>(
                (*m_allocator),
                [self, ctx, work = static_cast<Function<Status(JobContext&)>&&>(work)]()
                {
                    self->m_result = work ? work(*ctx) : Status{};
                    self->m_finished.store(true); // release: publishes m_result to the main thread
                });
        }

        UniquePtr<Thread> m_worker;
        UniquePtr<JobContext> m_ctx;     // the active job's context (the worker writes, the main thread reads)
        Function<void(Status)> m_onDone; // active job's completion (main thread)
        String m_title;                  // active job title (main thread)
        Atomic<bool> m_running{false};
        Atomic<bool> m_finished{false};
        Status m_result{};      // worker writes; main thread reads after m_finished (release/acquire)
        Array<Pending> m_queue; // main-thread only

        // Light lane (see SubmitLight).
        IAllocator* m_allocator;
        UniquePtr<Thread> m_lightWorker;
        Function<void()> m_lightOnDone; // main thread
        Atomic<bool> m_lightRunning{false};
        Atomic<bool> m_lightFinished{false};
        Array<PendingLight> m_lightQueue; // main-thread only
    };
}
