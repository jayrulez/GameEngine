// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :focus partition
//
// What a profiler page is looking at, held in one place and read by every view on the page: the
// source, the chosen frames (one or a range, from the frame strip), the chosen name (a scope or a
// GPU pass, from the timeline or the table) and a span marked on the timeline. A view sets these
// through the focus and draws what the focus holds. No view keeps a copy, so the strip, the
// counters, the timeline, the table and the detail stay in step, the way TimeAxis keeps a ruler
// and its views in step. Nothing here draws. The questions the views ask about the frames (a
// name's time in a frame, the longest and the median frame) are answered here, where tests can
// ask them too.

module;
#include "Core/Prelude.h"

export module foundation.profiler.gui:focus;

import foundation.core;
import foundation.profiler;

using namespace foundation::core;

export namespace foundation::profiler::gui
{
    /// A chosen name: a CPU scope or a GPU pass (the same id may name both).
    struct ChosenName
    {
        NameId name = 0;
        bool gpu = false;
        bool any = false;

        [[nodiscard]] bool Is(NameId other, bool otherGpu) const noexcept { return any && name == other && gpu == otherGpu; }
        friend bool operator==(const ChosenName&, const ChosenName&) = default;
    };

    /// A span marked on the timeline, in the source's ticks.
    struct MarkedSpan
    {
        u64 beginTick = 0;
        u64 endTick = 0;
        bool any = false;

        friend bool operator==(const MarkedSpan&, const MarkedSpan&) = default;
    };

    /// The notifications a focus sends to the views on it.
    class IProfileFocusClient
    {
    public:
        virtual ~IProfileFocusClient() = default;
        /// Called when the source or its frames changed (new frames recorded, another capture shown).
        virtual void OnProfileSourceChanged() {}
        /// Called when the chosen frames changed.
        virtual void OnProfileFramesChanged() {}
        /// Called when the chosen name or the marked span changed.
        virtual void OnProfileChoiceChanged() {}
    };

    class ProfileFocus : public RefCounted
    {
    public:
        explicit ProfileFocus(IAllocator& allocator) : m_clients(allocator) {}

        // === The source ===
        /// Sets the source the views read. It is not owned: it must outlive the focus or be set to
        /// null first. The choices are cleared, because their frames and names belong to the old
        /// source.
        void SetSource(const IProfileSource* source)
        {
            m_source = source;
            m_frames = FrameRange{};
            m_chosen = ChosenName{};
            m_span = MarkedSpan{};
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnProfileSourceChanged();
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnProfileFramesChanged();
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnProfileChoiceChanged();
        }
        [[nodiscard]] const IProfileSource* Source() const noexcept { return m_source; }
        /// Tells the views the source has new frames (or dropped old ones), so they read it again.
        void SourceGrew()
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnProfileSourceChanged();
        }

        // === The frames ===
        [[nodiscard]] FrameRange Frames() const noexcept { return m_frames; }
        /// Chooses frames; an empty range chooses none. The marked span is cleared, since it lay
        /// within the previous frames.
        void SetFrames(FrameRange frames)
        {
            if (frames.empty)
                frames = FrameRange{};
            if (frames.empty == m_frames.empty && frames.first == m_frames.first && frames.last == m_frames.last)
                return;
            m_frames = frames;
            const bool hadSpan = m_span.any;
            m_span = MarkedSpan{};
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnProfileFramesChanged();
            if (hadSpan)
                Chosen();
        }
        /// Chooses a single frame.
        void SetFrame(u64 frame)
        {
            FrameRange range;
            range.first = range.last = frame;
            range.empty = false;
            SetFrames(range);
        }

        // === The name and the span ===
        [[nodiscard]] ChosenName Name() const noexcept { return m_chosen; }
        void SetName(ChosenName name)
        {
            if (!name.any)
                name = ChosenName{};
            if (name == m_chosen)
                return;
            m_chosen = name;
            Chosen();
        }
        void ChooseName(NameId name, bool gpu) { SetName(ChosenName{name, gpu, true}); }
        void ClearName() { SetName(ChosenName{}); }

        [[nodiscard]] MarkedSpan Span() const noexcept { return m_span; }
        /// Marks the span [begin, end], given in either order; an empty span clears the mark.
        void SetSpan(u64 beginTick, u64 endTick)
        {
            MarkedSpan span;
            span.beginTick = Min(beginTick, endTick);
            span.endTick = Max(beginTick, endTick);
            span.any = span.endTick > span.beginTick;
            if (!span.any)
                span = MarkedSpan{};
            if (span == m_span)
                return;
            m_span = span;
            Chosen();
        }
        void ClearSpan() { SetSpan(0, 0); }

        // === Views ===
        void Join(IProfileFocusClient* client) { m_clients.PushBack(client); }
        void Leave(IProfileFocusClient* client)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                if (m_clients[i] == client)
                {
                    m_clients.RemoveAt(i);
                    return;
                }
        }
        [[nodiscard]] usize ClientCount() const noexcept { return m_clients.Size(); }

    private:
        void Chosen()
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnProfileChoiceChanged();
        }

        const IProfileSource* m_source = nullptr;
        FrameRange m_frames;
        ChosenName m_chosen;
        MarkedSpan m_span;
        Array<IProfileFocusClient*> m_clients; // non-owning; each view leaves when it is destroyed
    };

    /// Returns the ticks `name` took in `frame`: its CPU scopes on every thread, or its GPU passes.
    /// A scope nested inside another of the same name is counted once, through the outer one.
    [[nodiscard]] inline u64 NameTicksInFrame(const FrameRecord& frame, ChosenName name)
    {
        if (!name.any)
            return 0;
        u64 ticks = 0;
        if (name.gpu)
        {
            for (const GpuPassEvent& pass : frame.gpu)
                if (pass.name == name.name)
                    ticks += pass.endTick - pass.beginTick;
            return ticks;
        }
        for (const ThreadFrame& thread : frame.threads)
        {
            // Scopes are stored in the order they ended, so an outer scope of the name comes after
            // the inner ones it holds (a recursive scope). Each scope is counted unless a scope of
            // the same name ended later at a lesser depth around it.
            const Span<const ScopeEvent> scopes = thread.scopes.AsSpan();
            for (usize i = 0; i < scopes.Size(); ++i)
            {
                const ScopeEvent& scope = scopes[i];
                if (scope.name != name.name)
                    continue;
                bool inside = false;
                for (usize j = i + 1; j < scopes.Size() && !inside; ++j)
                    inside = scopes[j].name == name.name && scopes[j].depth < scope.depth &&
                             scopes[j].beginTick <= scope.beginTick && scope.endTick <= scopes[j].endTick;
                if (!inside)
                    ticks += scope.endTick - scope.beginTick;
            }
        }
        return ticks;
    }

    /// Returns the frame in `range` with the longest CPU time, or an empty range.
    [[nodiscard]] inline FrameRange LongestFrame(const IProfileSource& source, FrameRange range)
    {
        FrameRange longest;
        u64 most = 0;
        for (u64 n = range.first; !range.empty && n <= range.last; ++n)
            if (const FrameRecord* frame = source.Frame(n))
            {
                const u64 ticks = frame->endTick - frame->beginTick;
                if (longest.empty || ticks > most)
                {
                    most = ticks;
                    longest.first = longest.last = n;
                    longest.empty = false;
                }
            }
        return longest;
    }

    /// Returns the frame in `range` whose CPU time is the median, or an empty range. For an even
    /// count it is the shorter of the middle two.
    [[nodiscard]] inline FrameRange MedianFrame(const IProfileSource& source, FrameRange range, IAllocator& allocator)
    {
        struct Timed
        {
            u64 frame;
            u64 ticks;
        };
        Array<Timed> frames(allocator);
        for (u64 n = range.first; !range.empty && n <= range.last; ++n)
            if (const FrameRecord* frame = source.Frame(n))
                frames.PushBack(Timed{n, frame->endTick - frame->beginTick});
        FrameRange median;
        if (frames.IsEmpty())
            return median;
        frames.Sort([](const Timed& a, const Timed& b) { return a.ticks < b.ticks || (a.ticks == b.ticks && a.frame < b.frame); });
        median.first = median.last = frames[(frames.Size() - 1) / 2].frame;
        median.empty = false;
        return median;
    }

    /// Returns every frame the source holds, or an empty range.
    [[nodiscard]] inline FrameRange AllFrames(const IProfileSource* source) noexcept
    {
        FrameRange all;
        if (source == nullptr || !source->HasFrames())
            return all;
        all.first = source->OldestFrame();
        all.last = source->NewestFrame();
        all.empty = false;
        return all;
    }
}
