// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Project - :log_buffer partition.
//
// EditorLogBuffer: the log capture every editor-domain host uses - the editor application
// (its console panel), the MCP hosts (log_read), the cook and export tools. ONE thread-safe
// bounded ILogSink on core's GlobalLogger replaces Sedulous's logger+listener+buffer trio -
// a host never swaps the logger, it just adds a sink, so every LOG_* call across the engine is
// captured for free. Register it FIRST THING in main (before shell/device creation) so early
// startup logs are captured too.
//
// Entries keep full-fidelity heap strings (core's RingLogSink truncates messages to 192 chars -
// useless for build errors and file paths) and carry a monotonic sequence so main-thread
// consumers poll incrementally with CollectSince; when the ring is full the oldest entry drops
// but sequences keep advancing, so a consumer can tell (and report) that it missed entries.

module;
#include "Core/Prelude.h"

export module editor.project:log_buffer;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    struct EditorLogEntry
    {
        LogLevel level = LogLevel::Info;
        String category;
        String message;
        u64 sequence = 0; // monotonic, 1-based across the run
        Duration time;    // when it was written, since the buffer was made (the editor's start)
    };

    class EditorLogBuffer final : public ILogSink
    {
    public:
        explicit EditorLogBuffer(IAllocator& allocator, usize capacity = 4096)
            : m_entries(capacity, allocator), m_clock(Stopwatch::StartNew())
        {
        }

        EditorLogBuffer(const EditorLogBuffer&) = delete;
        EditorLogBuffer& operator=(const EditorLogBuffer&) = delete;

        // ILogSink - called from any thread (Logger::Dispatch serializes sinks, but consumers
        // read from the main thread concurrently, hence our own mutex).
        void Write(LogLevel level, StringView category, StringView message) noexcept override
        {
            ScopedLock lock(m_mutex);
            if (m_entries.IsFull())
            {
                EditorLogEntry discarded;
                (void)m_entries.PopFront(discarded);
                ++m_dropped;
            }
            EditorLogEntry entry;
            entry.level = level;
            entry.category = String(category);
            entry.message = String(message);
            entry.sequence = m_nextSequence++;
            entry.time = m_clock.Elapsed();
            (void)m_entries.PushBack(Move(entry));
        }

        /// Append every buffered entry with sequence > sinceSequence to `out` (oldest first).
        /// Returns the new high-water sequence to pass next time. Main-thread polling API.
        u64 CollectSince(u64 sinceSequence, Array<EditorLogEntry>& out) const
        {
            ScopedLock lock(m_mutex);
            u64 high = sinceSequence;
            for (usize i = 0; i < m_entries.Size(); ++i)
            {
                const EditorLogEntry& entry = m_entries[i];
                if (entry.sequence > sinceSequence)
                {
                    out.PushBack(entry);
                    high = entry.sequence;
                }
            }
            return high;
        }

        [[nodiscard]] usize Count() const
        {
            ScopedLock lock(m_mutex);
            return m_entries.Size();
        }

        /// The sequence of the most recently written entry (0 = nothing written yet).
        [[nodiscard]] u64 LatestSequence() const
        {
            ScopedLock lock(m_mutex);
            return m_nextSequence - 1;
        }

        /// Entries evicted before collection could see them (ring overflow): what a consumer
        /// reports as "N dropped".
        [[nodiscard]] u64 DroppedCount() const
        {
            ScopedLock lock(m_mutex);
            return m_dropped;
        }

        void Clear()
        {
            ScopedLock lock(m_mutex);
            EditorLogEntry discarded;
            while (m_entries.PopFront(discarded))
            {
            }
        }

    private:
        mutable Mutex m_mutex;
        RingBuffer<EditorLogEntry> m_entries;
        u64 m_nextSequence = 1;
        u64 m_dropped = 0;
        Stopwatch m_clock; // since it was made: each entry's time
    };
}
