// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:loads`: model files read on a worker thread. Reading the file is
// most of the cost of opening a model (a large FBX can take a tenth of a second or more, while the
// cook and bind after it take a few milliseconds), so the window requests the read here and keeps
// drawing. The main thread takes each finished read and completes the open (OpenModel::Finish),
// since the content database and the resources belong to it. Requests are read in order, and a
// path requested again while still queued is read once. Cancel drops the queue; a read already
// under way runs to completion and is then discarded, so nothing cancelled is handed over.

module;
#include "Core/Prelude.h"

export module modelviewer:loads;

import foundation.core;
import foundation.model;
import :library;

using namespace foundation::core;

export namespace modelviewer
{
    /// The result of one file read: the model and its stats, or an outcome other than Opened
    /// saying why the read failed.
    struct FileRead
    {
        String path;
        bool looseTextures = true;
        OpenModel::Outcome outcome = OpenModel::Outcome::NotFound;
        foundation::model::Model model;
        ModelStats stats;
    };

    class FileLoads
    {
    public:
        explicit FileLoads(IAllocator& allocator) : m_queue(allocator), m_done(allocator)
        {
            m_thread = Thread([this]() { Work(); });
        }
        FileLoads(const FileLoads&) = delete;
        FileLoads& operator=(const FileLoads&) = delete;
        ~FileLoads()
        {
            {
                ScopedLock lock(m_mutex);
                m_stopping = true;
                m_queue.Clear();
            }
            m_wake.NotifyAll();
            m_thread.Join();
        }

        /// Queues `path` behind what is already queued. A path that is already queued keeps its place.
        void Request(StringView path, bool looseTextures)
        {
            {
                ScopedLock lock(m_mutex);
                for (const Queued& p : m_queue)
                    if (p.path.AsView() == path && p.looseTextures == looseTextures)
                        return;
                m_queue.PushBack(Queued{String(path), looseTextures});
            }
            m_wake.NotifyOne();
        }

        /// Drops everything queued, the finished reads not yet taken, and the read under way (if
        /// any) once it ends.
        void Cancel()
        {
            ScopedLock lock(m_mutex);
            m_queue.Clear();
            m_done.Clear();
            ++m_generation;
        }

        /// Moves the oldest finished read into `out`; returns false when none is waiting.
        bool Take(FileRead& out)
        {
            ScopedLock lock(m_mutex);
            if (m_done.IsEmpty())
                return false;
            out = foundation::core::Move(m_done[0]);
            m_done.RemoveAt(0);
            return true;
        }

        /// The number of reads queued or under way, not counting finished reads waiting to be taken.
        [[nodiscard]] usize Pending() const
        {
            ScopedLock lock(m_mutex);
            return m_queue.Size() + (m_reading ? 1u : 0u);
        }
        /// Whether `path` is queued or being read.
        [[nodiscard]] bool IsPending(StringView path) const
        {
            ScopedLock lock(m_mutex);
            if (m_reading && m_readingPath.AsView() == path)
                return true;
            for (const Queued& p : m_queue)
                if (p.path.AsView() == path)
                    return true;
            return false;
        }

    private:
        struct Queued
        {
            String path;
            bool looseTextures = true;
        };

        void Work()
        {
            for (;;)
            {
                Queued next;
                u64 generation = 0;
                {
                    ScopedLock lock(m_mutex);
                    while (!m_stopping && m_queue.IsEmpty())
                        m_wake.Wait(m_mutex);
                    if (m_stopping)
                        return;
                    next = foundation::core::Move(m_queue[0]);
                    m_queue.RemoveAt(0);
                    m_reading = true;
                    m_readingPath = next.path;
                    generation = m_generation;
                }
                FileRead read;
                read.path = next.path;
                read.looseTextures = next.looseTextures;
                read.outcome = OpenModel::LoadFile(next.path.AsView(), next.looseTextures, read.model, read.stats);
                ScopedLock lock(m_mutex);
                m_reading = false;
                m_readingPath = {};
                if (generation == m_generation && !m_stopping)
                    m_done.PushBack(foundation::core::Move(read));
            }
        }

        mutable Mutex m_mutex;
        ConditionVariable m_wake;
        Array<Queued> m_queue;
        Array<FileRead> m_done;
        bool m_reading = false;
        String m_readingPath;
        u64 m_generation = 0;
        bool m_stopping = false;
        Thread m_thread; // declared last so the thread starts after the other members are constructed
    };
}
