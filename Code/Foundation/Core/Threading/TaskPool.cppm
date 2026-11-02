// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :task_pool partition
//
// Long work off the frame, handed back on the main thread (Specs/task-layer.md). The JobSystem is
// for the frame's fan-out; a cook, an import read, a decode or an export runs here instead, on
// workers of the pool's own, so it never holds a worker the frame's ParallelFor needs.
//
// A task is a name and work, Function<Status(TaskContext&)>, with an optional completion. Submit
// returns its TaskHandle: its state, its progress, and Cancel(). A task goes to a lane: tasks of one
// lane run one at a time in the order submitted (a lane is a queue over the workers, not a thread),
// lanes run side by side, and the shared lane runs its tasks side by side too.
//
// The owner calls Pump() once a frame on the main thread: it runs the completions of the tasks
// finished since, in finishing order, and hands out their log lines. A cancelled task's completion
// never runs, so an owner that cancels its tasks on its way out is never called back.
//
// Cancelling is cooperative: a queued task is dropped without running; a running one sees
// IsCancelled() at its next check and returns ErrorCode::Cancelled. Cancel never waits; Wait() does,
// for the main thread that must have a task gone before it goes on.
//
// The work owns what it uses (captured by reference count, never a raw owner). A worker never
// touches what the main thread changes; it writes its result where its completion reads it.
//
// A pool of no workers (the web build, which has no threads) runs its queued tasks in Pump, one
// at a time within a time budget, so the page draws between tasks.

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"
#include <atomic>

export module foundation.core:task_pool;

import :base;
import :allocator;
import :ref_counted;
import :array;
import :function;
import :string;
import :system;
import :time;
import :thread;
import :mutex;
import :condition_variable;
import :scoped_lock;
import :cancel;
import :job_system; // CurrentJobHooks: the profiler sees a task as it sees a job

export namespace foundation::core
{
    class TaskPool;
    class TaskContext;

    /// Where a task runs. Shared runs its tasks side by side; a lane from CreateLane runs one at a time.
    enum class TaskLane : u32
    {
        Shared = 0,
    };

    enum class TaskState : u8
    {
        Queued,
        Running,
        Done,      ///< its work returned success
        Failed,    ///< its work returned an error
        Cancelled, ///< dropped before it ran, or it stopped when told
    };

    /// How far a task has got, as its work last said.
    struct TaskProgress
    {
        f32 fraction = -1.0f; ///< 0 to 1; below 0 when the work has not said
        String step;          ///< what it is doing now ("Writing Rider.glb")
    };

    /// One submitted task: its state, its progress, and how to cancel it. Reference-counted, so
    /// whoever started it may keep it as long as it likes.
    class TaskHandle final : public RefCounted
    {
    public:
        TaskHandle(IAllocator& allocator, StringView name, TaskLane lane, Function<Status(TaskContext&)> work, Function<void(Status)> done)
            : m_allocator(&allocator), m_name(name), m_lane(lane), m_work(static_cast<Function<Status(TaskContext&)>&&>(work)),
              m_done(static_cast<Function<void(Status)>&&>(done)), m_cancel(allocator), m_log(allocator)
        {
        }

        [[nodiscard]] StringView Name() const noexcept { return m_name.AsView(); }
        [[nodiscard]] TaskLane Lane() const noexcept { return m_lane; }
        [[nodiscard]] TaskState State() const noexcept { return static_cast<TaskState>(m_state.load(std::memory_order_acquire)); }
        [[nodiscard]] bool IsEnded() const noexcept
        {
            const TaskState state = State();
            return state == TaskState::Done || state == TaskState::Failed || state == TaskState::Cancelled;
        }
        /// What its work returned; meaningful once IsEnded.
        [[nodiscard]] Status Result() const noexcept { return m_result; }

        [[nodiscard]] TaskProgress Progress() const
        {
            ScopedLock lock(m_lock);
            return m_progress;
        }

        /// Tells it to stop: dropped if it has not started, told through its token if it has. Its
        /// completion will not run. Never waits (TaskPool::Wait does).
        void Cancel() noexcept { m_cancel.Cancel(); }
        [[nodiscard]] bool IsCancelled() const noexcept { return m_cancel.IsCancelled(); }
        [[nodiscard]] CancelToken Token() const noexcept { return m_cancel.Token(); }

    private:
        friend class TaskPool;
        friend class TaskContext;

        void SetState(TaskState state) noexcept { m_state.store(static_cast<u8>(state), std::memory_order_release); }

        IAllocator* m_allocator;
        String m_name;
        TaskLane m_lane;
        Function<Status(TaskContext&)> m_work;
        Function<void(Status)> m_done;
        CancelSource m_cancel;
        std::atomic<u8> m_state{static_cast<u8>(TaskState::Queued)};
        Status m_result;
        mutable Mutex m_lock; // guards m_progress and m_log, written by the worker
        TaskProgress m_progress;
        Array<String> m_log;
    };

    /// What the work is given: its cancel token, and where it says how far it has got. Safe from
    /// the worker; nothing here touches the main thread.
    class TaskContext
    {
    public:
        [[nodiscard]] bool IsCancelled() const noexcept { return m_task->IsCancelled(); }
        [[nodiscard]] CancelToken Token() const noexcept { return m_task->Token(); }
        void SetProgress(f32 fraction, StringView step = {})
        {
            ScopedLock lock(m_task->m_lock);
            m_task->m_progress.fraction = fraction;
            m_task->m_progress.step = String(step);
        }
        /// A line for whoever pumps the pool (the editor's Console).
        void Log(StringView line)
        {
            ScopedLock lock(m_task->m_lock);
            m_task->m_log.PushBack(String(line));
        }

    private:
        friend class TaskPool;
        explicit TaskContext(TaskHandle& task) noexcept : m_task(&task) {}
        TaskHandle* m_task;
    };

    class TaskPool
    {
    public:
        /// The workers for long work: the logical cores less two (the frame keeps its threads), at
        /// least one and at most four (a big cook should not flood the disk and memory); none on the
        /// web build, which has no threads.
        [[nodiscard]] static u32 DefaultWorkerCount() noexcept
        {
#if PLATFORM_WEB
            return 0u;
#else
            const u32 cores = LogicalCoreCount();
            const u32 spare = cores > 2u ? cores - 2u : 1u;
            return spare > 4u ? 4u : spare;
#endif
        }

        /// `workerCount` 0 runs every task in Pump on the calling thread. The workers start with the
        /// first task, so a pool nothing uses costs no threads.
        TaskPool(IAllocator& allocator, u32 workerCount)
            : m_allocator(&allocator), m_workerCount(workerCount), m_lanes(allocator), m_runnable(allocator), m_running(allocator),
              m_finished(allocator), m_workers(allocator)
        {
            m_lanes.PushBack(Lane{String(u8"Shared"), Array<RefPtr<TaskHandle>>(allocator), false}); // TaskLane::Shared, never busy
        }

        TaskPool(const TaskPool&) = delete;
        TaskPool& operator=(const TaskPool&) = delete;

        /// Cancels everything: what is queued is dropped, what runs is told, and waited for. No
        /// completion runs.
        ~TaskPool()
        {
            {
                ScopedLock lock(m_mutex);
                m_stopping = true;
                for (const RefPtr<TaskHandle>& task : m_running)
                    task->Cancel();
                DropQueued(m_runnable);
                for (Lane& lane : m_lanes)
                    DropQueued(lane.queued);
            }
            m_wake.NotifyAll();
            for (Thread& worker : m_workers)
                worker.Join();
        }

        /// A lane whose tasks run one at a time, in the order submitted (the editor's build lane:
        /// nothing it runs races the content databases).
        [[nodiscard]] TaskLane CreateLane(StringView name)
        {
            ScopedLock lock(m_mutex);
            m_lanes.PushBack(Lane{String(name), Array<RefPtr<TaskHandle>>(*m_allocator), false});
            return static_cast<TaskLane>(m_lanes.Size() - 1);
        }

        /// Queues `work` on `lane`; `done` runs in Pump on the main thread once it has ended, unless
        /// it was cancelled. Called on the main thread (or from a completion).
        RefPtr<TaskHandle> Submit(StringView name, Function<Status(TaskContext&)> work, Function<void(Status)> done = {},
                                  TaskLane lane = TaskLane::Shared)
        {
            RefPtr<TaskHandle> task = MakeRef<TaskHandle>(*m_allocator, *m_allocator, name, lane, static_cast<Function<Status(TaskContext&)>&&>(work),
                                                          static_cast<Function<void(Status)>&&>(done));
            if (m_workers.Size() < m_workerCount)
                for (u32 i = static_cast<u32>(m_workers.Size()); i < m_workerCount; ++i)
                    m_workers.PushBack(Thread([this]() { WorkerLoop(); }));
            {
                ScopedLock lock(m_mutex);
                DIAGNOSTIC_ASSERT(static_cast<usize>(lane) < m_lanes.Size());
                Lane& queue = m_lanes[static_cast<usize>(lane)];
                if (lane == TaskLane::Shared || !queue.busy)
                {
                    queue.busy = lane != TaskLane::Shared;
                    m_runnable.PushBack(task);
                }
                else
                {
                    queue.queued.PushBack(task); // after the one it runs now
                }
            }
            m_wake.NotifyOne();
            return task;
        }

        /// The main thread's once-a-frame call: the log lines written since, then the completions
        /// of the tasks ended since, in the order they ended. A pool of no workers first runs its
        /// queued tasks here, one at a time, for as long as `budget` allows (at least one).
        void Pump(const Function<void(const TaskHandle&, StringView)>& log = {}, Duration budget = Duration::FromMilliseconds(8.0))
        {
            if (m_workerCount == 0u)
            {
                const Stopwatch clock = Stopwatch::StartNew();
                do
                {
                    if (!RunOneInline())
                        break;
                } while (clock.Elapsed() < budget);
            }

            Array<RefPtr<TaskHandle>> running(*m_allocator);
            Array<RefPtr<TaskHandle>> finished(*m_allocator);
            {
                ScopedLock lock(m_mutex);
                running = m_running;
                finished = static_cast<Array<RefPtr<TaskHandle>>&&>(m_finished);
                m_finished = Array<RefPtr<TaskHandle>>(*m_allocator);
            }
            for (const RefPtr<TaskHandle>& task : running)
                HandOutLog(*task, log);
            for (const RefPtr<TaskHandle>& task : finished)
            {
                HandOutLog(*task, log);
                if (!task->IsCancelled() && task->m_done)
                    task->m_done(task->m_result); // may Submit more
                task->m_done = {};                // what it captured goes now, on the main thread
            }
        }

        /// Waits on the calling thread for `task` to end. A task cancelled before it started ends at
        /// once; a running one ends at its next check. A pool of no workers runs the queue until it
        /// has. Its completion still runs in the next Pump (unless it was cancelled).
        void Wait(TaskHandle& task)
        {
            for (;;)
            {
                {
                    ScopedLock lock(m_mutex);
                    if (task.IsEnded())
                        return;
                    if (task.State() == TaskState::Queued && task.IsCancelled() && DropIfQueued(task))
                        return;
                    if (m_workerCount != 0u)
                    {
                        while (!task.IsEnded())
                            m_ended.Wait(m_mutex);
                        return;
                    }
                }
                if (!RunOneInline())
                    return; // nothing left to run: it was never this pool's
            }
        }

        /// Waits for every task to end, then pumps: the completions run here (tests; a host that
        /// must have its work done before it goes on). A pool of no workers runs them all here.
        void Finish(const Function<void(const TaskHandle&, StringView)>& log = {})
        {
            if (m_workerCount == 0u)
            {
                while (RunOneInline())
                {
                }
            }
            else
            {
                ScopedLock lock(m_mutex);
                while (!m_runnable.IsEmpty() || !m_running.IsEmpty() || AnyLaneQueued())
                    m_ended.Wait(m_mutex);
            }
            Pump(log);
        }

        /// Tasks submitted and not yet handed back by Pump.
        [[nodiscard]] u32 InFlight() const
        {
            ScopedLock lock(m_mutex);
            usize count = m_runnable.Size() + m_running.Size() + m_finished.Size();
            for (const Lane& lane : m_lanes)
                count += lane.queued.Size();
            return static_cast<u32>(count);
        }

        /// Whether `lane` has a task queued or running (the editor gates project changes on its
        /// build lane).
        [[nodiscard]] bool IsLaneBusy(TaskLane lane) const
        {
            ScopedLock lock(m_mutex);
            return static_cast<usize>(lane) < m_lanes.Size() && m_lanes[static_cast<usize>(lane)].busy;
        }

        /// The task `lane` runs now (null when none): what a progress view shows.
        [[nodiscard]] RefPtr<TaskHandle> RunningIn(TaskLane lane) const
        {
            ScopedLock lock(m_mutex);
            for (const RefPtr<TaskHandle>& task : m_running)
                if (task->Lane() == lane)
                    return task;
            return {};
        }

        [[nodiscard]] u32 WorkerCount() const noexcept { return m_workerCount; }

    private:
        struct Lane
        {
            String name;
            Array<RefPtr<TaskHandle>> queued; // behind the one it has runnable or running
            bool busy = false;                // a task of it runnable or running
        };

        [[nodiscard]] bool AnyLaneQueued() const
        {
            for (const Lane& lane : m_lanes)
                if (!lane.queued.IsEmpty())
                    return true;
            return false;
        }

        static void DropQueued(Array<RefPtr<TaskHandle>>& queue)
        {
            for (const RefPtr<TaskHandle>& task : queue)
            {
                task->Cancel();
                task->m_result = Status(ErrorCode::Cancelled);
                task->SetState(TaskState::Cancelled);
            }
            queue.Clear();
        }

        // Takes the next runnable task (m_mutex held): one cancelled before it started ends here,
        // unrun, and the next is taken. Null when none is runnable.
        RefPtr<TaskHandle> TakeRunnable()
        {
            while (!m_runnable.IsEmpty())
            {
                RefPtr<TaskHandle> task = m_runnable[0];
                m_runnable.RemoveAt(0);
                if (task->IsCancelled())
                {
                    End(task, Status(ErrorCode::Cancelled));
                    continue;
                }
                task->SetState(TaskState::Running);
                m_running.PushBack(task);
                return task;
            }
            return {};
        }

        // Runs `task`'s work on this thread, with the profiler's hooks around it.
        static Status Run(TaskHandle& task)
        {
            const JobHooks* hooks = CurrentJobHooks();
            const bool began = hooks != nullptr && hooks->begin != nullptr && hooks->begin();
            TaskContext context(task);
            const Status result = task.m_work(context);
            task.m_work = {}; // what it captured goes here, on the thread that ran it
            if (began && hooks->end != nullptr)
                hooks->end();
            return result;
        }

        // Records how `task` ended and lets its lane run its next (m_mutex held).
        void End(const RefPtr<TaskHandle>& task, Status result)
        {
            task->m_result = result;
            task->SetState(result.Code() == ErrorCode::Cancelled ? TaskState::Cancelled
                           : result.IsOk()                       ? TaskState::Done
                                                                 : TaskState::Failed);
            for (usize i = 0; i < m_running.Size(); ++i)
                if (m_running[i].Get() == task.Get())
                {
                    m_running.RemoveAt(i);
                    break;
                }
            m_finished.PushBack(task);
            if (task->Lane() != TaskLane::Shared)
            {
                Lane& lane = m_lanes[static_cast<usize>(task->Lane())];
                if (lane.queued.IsEmpty())
                    lane.busy = false;
                else
                {
                    m_runnable.PushBack(lane.queued[0]);
                    lane.queued.RemoveAt(0);
                    m_wake.NotifyOne();
                }
            }
            m_ended.NotifyAll();
        }

        // Ends a cancelled `task` that has not started, wherever it waits (m_mutex held).
        bool DropIfQueued(TaskHandle& task)
        {
            const auto take = [&task](Array<RefPtr<TaskHandle>>& queue) -> RefPtr<TaskHandle>
            {
                for (usize i = 0; i < queue.Size(); ++i)
                    if (queue[i].Get() == &task)
                    {
                        RefPtr<TaskHandle> found = queue[i];
                        queue.RemoveAt(i);
                        return found;
                    }
                return {};
            };
            if (RefPtr<TaskHandle> found = take(m_runnable))
            {
                End(found, Status(ErrorCode::Cancelled)); // its lane's next becomes runnable
                return true;
            }
            for (Lane& lane : m_lanes)
                if (RefPtr<TaskHandle> found = take(lane.queued))
                {
                    found->m_result = Status(ErrorCode::Cancelled);
                    found->SetState(TaskState::Cancelled);
                    m_finished.PushBack(found);
                    m_ended.NotifyAll();
                    return true;
                }
            return false;
        }

        // A pool of no workers: runs the next runnable task here. False when none was.
        bool RunOneInline()
        {
            RefPtr<TaskHandle> task;
            {
                ScopedLock lock(m_mutex);
                task = TakeRunnable();
            }
            if (!task)
                return false;
            const Status result = Run(*task);
            ScopedLock lock(m_mutex);
            End(task, result);
            return true;
        }

        void WorkerLoop()
        {
            for (;;)
            {
                RefPtr<TaskHandle> task;
                {
                    ScopedLock lock(m_mutex);
                    for (;;)
                    {
                        task = TakeRunnable();
                        if (task || m_stopping)
                            break;
                        m_wake.Wait(m_mutex);
                    }
                }
                if (!task)
                    return;
                const Status result = Run(*task);
                ScopedLock lock(m_mutex);
                End(task, result);
            }
        }

        static void HandOutLog(TaskHandle& task, const Function<void(const TaskHandle&, StringView)>& log)
        {
            Array<String> lines(*task.m_allocator);
            {
                ScopedLock lock(task.m_lock);
                if (task.m_log.IsEmpty())
                    return;
                lines = static_cast<Array<String>&&>(task.m_log);
                task.m_log = Array<String>(*task.m_allocator);
            }
            if (log)
                for (const String& line : lines)
                    log(task, line.AsView());
        }

        IAllocator* m_allocator;
        u32 m_workerCount;
        mutable Mutex m_mutex; // guards everything below but the workers
        ConditionVariable m_wake;  // a task became runnable, or the pool stops
        ConditionVariable m_ended; // a task ended (Wait)
        Array<Lane> m_lanes;
        Array<RefPtr<TaskHandle>> m_runnable; // ready for a worker, in order
        Array<RefPtr<TaskHandle>> m_running;
        Array<RefPtr<TaskHandle>> m_finished; // ended, for Pump
        bool m_stopping = false;
        Array<Thread> m_workers;
    };
}
