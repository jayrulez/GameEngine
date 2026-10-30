// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ModelViewer - `modelviewer:loads`: model files read on a thread of their own
// (Documentation/Specs/model-viewer.md, P10). Reading a file is most of an open (a large FBX
// takes a tenth of a second or more; the cook and the bind after it a few milliseconds), so the
// window asks for the read here and goes on drawing; the main thread takes what has been read
// and finishes the open (OpenModel::Finish), the content database and the resources staying on
// it. Requests are read in order; a path asked again while queued is not read twice; Cancel drops
// what is queued, and a read already under way is let finish and dropped, so nothing cancelled is
// handed over.

module;
#include "Core/Prelude.h"

export module modelviewer:loads;

import foundation.core;
import foundation.model;
import :library;

using namespace foundation::core;

export namespace modelviewer
{
    /// One file read: its model and numbers, or why not (outcome other than Opened).
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

        /// `path` read next after what is queued (once: asked again while queued, it stays where it is).
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

        /// Everything queued dropped, and the read under way (if any) dropped when it ends; what
        /// was read already and not taken is dropped too.
        void Cancel()
        {
            ScopedLock lock(m_mutex);
            m_queue.Clear();
            m_done.Clear();
            ++m_generation;
        }

        /// A finished read into `out` (oldest first); false when none is waiting.
        bool Take(FileRead& out)
        {
            ScopedLock lock(m_mutex);
            if (m_done.IsEmpty())
                return false;
            out = foundation::core::Move(m_done[0]);
            m_done.RemoveAt(0);
            return true;
        }

        /// Reads queued or under way (not counting those finished and waiting to be taken).
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
        Thread m_thread; // last: it starts once the rest is made
    };
}
