// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:loads`: model files read off the main thread, and cooked there too when
// given the open model's cache, on a task pool (Specs/task-layer.md): the loads' own, or one a window
// shares between its loads so no two cooks run into one cache folder at once. Reading the file is most of the cost of opening a model (a large
// FBX can take a tenth of a second or more, while the cook and bind after it take a few
// milliseconds), so the window requests the read here and keeps drawing. The main thread takes each
// finished read and completes the open (editor::OpenModel::Bind when the worker cooked it, else
// Finish), since the resources belong to it. One worker reads them in the order asked, and a path requested again
// while still queued is read once. Cancel drops the queue; a read already under way runs to
// completion and is then discarded, so nothing cancelled is handed over.

module;
#include "Core/Prelude.h"

export module modelviewer:loads;

import foundation.core;
import foundation.model;
import editor.modelpreview;

using namespace foundation::core;

export namespace modelviewer
{
    /// The result of one file read: the model and its stats, or an outcome other than Opened
    /// saying why the read failed.
    struct FileRead
    {
        String path;
        bool looseTextures = true;
        editor::OpenModel::Outcome outcome = editor::OpenModel::Outcome::NotFound;
        foundation::model::Model model;
        editor::ModelStats stats;
        Guid cooked; // the model the worker cooked into the cache it was given, to Bind; nil when not cooked
    };

    class FileLoads
    {
    public:
        /// Reads on a one-worker pool of its own.
        explicit FileLoads(IAllocator& allocator)
            : m_allocator(&allocator), m_requests(allocator), m_ending(allocator), m_done(allocator), m_own(MakeUnique<TaskPool>(allocator, allocator, 1u))
        {
            m_tasks = m_own.Get();
        }
        /// Reads on `shared`, which outlives this, beside whatever else submits to it.
        FileLoads(IAllocator& allocator, TaskPool& shared)
            : m_allocator(&allocator), m_requests(allocator), m_ending(allocator), m_done(allocator), m_tasks(&shared)
        {
        }
        FileLoads(const FileLoads&) = delete;
        FileLoads& operator=(const FileLoads&) = delete;
        /// Its reads end before it goes: what is queued is dropped, a read under way is waited for,
        /// and no completion reaches it after.
        ~FileLoads()
        {
            Cancel();
            for (const Requested& r : m_ending)
                m_tasks->Wait(*r.task);
        }

        /// Queues `path` behind what is already queued. A path that is already queued keeps its place.
        /// Given `cook` (the open model's cache, OpenModel::Cache), the worker cooks what it read into
        /// it as well, and the read's `cooked` names the model to Bind.
        void Request(StringView path, bool looseTextures, RefPtr<editor::ModelCache> cook = {})
        {
            for (const Requested& r : m_requests)
                if (r.task->State() == TaskState::Queued && r.path.AsView() == path && r.looseTextures == looseTextures)
                    return;
            RefPtr<Slot> slot = MakeRef<Slot>(*m_allocator);
            slot->read.path = String(path);
            slot->read.looseTextures = looseTextures;
            FileLoads* self = this;
            RefPtr<TaskHandle> task = m_tasks->Submit(
                Format(u8"Reading {}", path).AsView(),
                [slot, cook](TaskContext& context)
                {
                    FileRead& read = slot->read;
                    read.outcome = editor::OpenModel::LoadFile(read.path.AsView(), read.looseTextures, read.model, read.stats);
                    if (read.outcome == editor::OpenModel::Outcome::Opened && cook)
                        read.outcome = editor::OpenModel::Cook(*cook, read.model, read.stats, read.cooked, context.Token());
                    return read.outcome == editor::OpenModel::Outcome::Cancelled ? Status(ErrorCode::Cancelled) : Status{};
                },
                [self, slot](Status) { self->m_done.PushBack(foundation::core::Move(slot->read)); }); // in Take, on the main thread
            m_requests.PushBack(Requested{String(path), looseTextures, foundation::core::Move(task)});
        }

        /// Drops everything queued, the finished reads not yet taken, and the read under way (if
        /// any) once it ends.
        void Cancel()
        {
            for (Requested& r : m_requests)
            {
                r.task->Cancel();
                if (!r.task->IsEnded())
                    m_ending.PushBack(foundation::core::Move(r)); // waited for if this goes first
            }
            m_requests.Clear();
            m_done.Clear();
        }

        /// Moves the oldest finished read into `out`; returns false when none is waiting.
        bool Take(FileRead& out)
        {
            m_tasks->Pump(); // the reads finished since: their completions queue them in m_done
            Forget();
            if (m_done.IsEmpty())
                return false;
            out = foundation::core::Move(m_done[0]);
            m_done.RemoveAt(0);
            return true;
        }

        /// The number of reads queued or under way, not counting finished reads waiting to be taken.
        [[nodiscard]] usize Pending() const
        {
            usize count = 0;
            for (const Requested& r : m_requests)
                if (!r.task->IsEnded())
                    ++count;
            return count;
        }
        /// Whether `path` is queued or being read.
        [[nodiscard]] bool IsPending(StringView path) const
        {
            for (const Requested& r : m_requests)
                if (!r.task->IsEnded() && r.path.AsView() == path)
                    return true;
            return false;
        }

    private:
        // What a read fills and its completion hands over, held by both closures.
        struct Slot final : RefCounted
        {
            FileRead read;
        };
        struct Requested
        {
            String path;
            bool looseTextures = true;
            RefPtr<TaskHandle> task;
        };

        // Drops the requests that have ended (their reads are in m_done, or were cancelled).
        void Forget()
        {
            for (usize i = m_requests.Size(); i > 0; --i)
                if (m_requests[i - 1].task->IsEnded())
                    m_requests.RemoveAt(i - 1);
            for (usize i = m_ending.Size(); i > 0; --i)
                if (m_ending[i - 1].task->IsEnded())
                    m_ending.RemoveAt(i - 1);
        }

        IAllocator* m_allocator;
        Array<Requested> m_requests;
        Array<Requested> m_ending; // cancelled while running: waited for if this goes before they end
        Array<FileRead> m_done;
        TaskPool* m_tasks = nullptr;
        UniquePtr<TaskPool> m_own; // declared last, so destroyed first when the pool is its own
    };
}
