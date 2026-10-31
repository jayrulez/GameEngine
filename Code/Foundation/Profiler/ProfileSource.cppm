// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Profiler - :source partition.
///
/// What every profiler view reads: frames by number and the tables their ids point into. A source
/// is the live store in this process or a capture read from a file (:stream); a running player
/// read over the network could be one too without any view changing.

module;
#include "Core/Prelude.h"

export module foundation.profiler:source;

import foundation.core;
import :store;

using namespace foundation::core;

export namespace foundation::profiler
{
    class IProfileSource
    {
    public:
        virtual ~IProfileSource() = default;

        /// Ticks a second of the clock the frames' ticks are on (the recording process's).
        [[nodiscard]] virtual u64 TickFrequency() const noexcept = 0;

        /// Whether it holds any frame; then they lie between OldestFrame and NewestFrame, a
        /// number in between possibly without a record (Frame answers null).
        [[nodiscard]] virtual bool HasFrames() const noexcept = 0;
        [[nodiscard]] virtual u64 OldestFrame() const noexcept = 0;
        [[nodiscard]] virtual u64 NewestFrame() const noexcept = 0;
        [[nodiscard]] virtual const FrameRecord* Frame(u64 frameNumber) const noexcept = 0;

        [[nodiscard]] virtual StringView NameOf(NameId id) const noexcept = 0;
        [[nodiscard]] virtual u32 NameCount() const noexcept = 0;
        [[nodiscard]] virtual Span<const ThreadInfo> Threads() const noexcept = 0;
        [[nodiscard]] virtual Span<const CounterInfo> Counters() const noexcept = 0;
    };

    /// The store in this process, read on its frame thread.
    class LiveProfileSource final : public IProfileSource
    {
    public:
        explicit LiveProfileSource(const Profiler& profiler) noexcept : m_profiler(&profiler) {}

        u64 TickFrequency() const noexcept override { return GetTickFrequency(); }
        bool HasFrames() const noexcept override { return m_profiler->HasFrames(); }
        u64 OldestFrame() const noexcept override { return m_profiler->OldestKeptFrame(); }
        u64 NewestFrame() const noexcept override { return m_profiler->LatestRecordedFrame(); }
        const FrameRecord* Frame(u64 frameNumber) const noexcept override
        {
            return m_profiler->Frame(frameNumber);
        }
        StringView NameOf(NameId id) const noexcept override { return m_profiler->NameOf(id); }
        u32 NameCount() const noexcept override { return m_profiler->NameCount(); }
        Span<const ThreadInfo> Threads() const noexcept override { return m_profiler->Threads(); }
        Span<const CounterInfo> Counters() const noexcept override { return m_profiler->Counters(); }

    private:
        const Profiler* m_profiler;
    };
}
