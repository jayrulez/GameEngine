// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :frame_strip partition
//
// The frames, side by side (X7 of Specs/editor-gui.md): the frame strip and the counters' plots
// under it, on one FrameAxis so they show the same frames at the same places. The strip's bars
// are each frame's CPU time against a budget (within it, over it, over twice it), on a scale with
// its values printed (twice the budget; the frames shown's longest without one), a bar past the
// scale clipped and marked; the frames chosen in the accent, boxed; the name chosen (a scope or
// a pass) drawn inside each bar as its share of the frame. A counter's plot is its value each
// frame between the least and the most shown, which it prints. More frames than pixels share a
// column, which shows the longest of them (a hitch never hidden by its neighbours).
//
// Mouse first, on any of them: a click chooses a frame, a drag a range; the wheel zooms about
// the pointer and a middle drag pans; a double click shows every frame again. While the axis
// fits (until a zoom or a pan) it follows the source's frames as they grow.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.profiler.gui:frame_strip;

import foundation.core;
import foundation.vg;
import foundation.gui;
import foundation.profiler;
import :focus;
import :widget;

using namespace foundation::core;
using namespace foundation::gui;

export namespace foundation::profiler::gui
{
    /// What a frame axis tells the views on it.
    class IFrameAxisClient
    {
    public:
        virtual ~IFrameAxisClient() = default;
        virtual void OnFrameAxisChanged() = 0;
    };

    /// The frames a strip and its plots show: the frame at their start and how many across.
    class FrameAxis : public RefCounted
    {
    public:
        /// Never fewer frames across than this (a zoom stops there).
        static constexpr f64 kMinFrames = 8.0;

        explicit FrameAxis(IAllocator& allocator) : m_clients(allocator) {}

        /// The frame number at the views' start (fractional: a pan moves by less than a frame).
        [[nodiscard]] f64 First() const noexcept { return m_first; }
        /// How many frames lie across the views.
        [[nodiscard]] f64 Count() const noexcept { return m_count; }
        /// Whether it shows every frame of the source, following them as they grow.
        [[nodiscard]] bool Fits() const noexcept { return m_fits; }

        /// Shows [oldest, newest], and keeps doing so as Track is told of more.
        void Fit(u64 oldest, u64 newest)
        {
            m_fits = true;
            Show(static_cast<f64>(oldest), static_cast<f64>(Max(newest, oldest) - oldest + 1));
        }
        /// The source's frames now: while it fits, it shows them all.
        void Track(const IProfileSource* source)
        {
            m_oldest = source != nullptr && source->HasFrames() ? static_cast<f64>(source->OldestFrame()) : 0.0;
            m_newest = source != nullptr && source->HasFrames() ? static_cast<f64>(source->NewestFrame()) : 0.0;
            m_any = source != nullptr && source->HasFrames();
            if (m_fits)
                Show(m_oldest, m_any ? m_newest - m_oldest + 1.0 : 1.0);
        }
        /// Zooms by `factor` (above 1 in) about `frame`, which stays where it is: no closer than
        /// kMinFrames across, no further than every frame.
        void ZoomAbout(f64 frame, f64 factor)
        {
            if (factor <= 0.0)
                return;
            const f64 along = (frame - m_first) / m_count;
            const f64 most = m_any ? Max(m_newest - m_oldest + 1.0, kMinFrames) : m_count;
            const f64 count = Clamp(m_count / factor, kMinFrames, most);
            m_fits = false;
            Show(frame - along * count, count);
        }
        /// Moves by `frames` (positive shows later ones), kept over the source's frames.
        void PanBy(f64 frames)
        {
            m_fits = false;
            Show(m_first + frames, m_count);
        }

        /// The whole frame number at or below `frame` (frame numbers are far past f32's whole range).
        [[nodiscard]] static f64 Whole(f64 frame) noexcept
        {
            const f64 truncated = static_cast<f64>(static_cast<i64>(frame));
            return truncated > frame ? truncated - 1.0 : truncated;
        }

        /// The frame at `offset` pixels into a view `width` wide, and where a frame's start is.
        [[nodiscard]] f64 FrameAt(f32 offset, f32 width) const noexcept
        {
            return m_first + static_cast<f64>(offset) / static_cast<f64>(Max(width, 1.0f)) * m_count;
        }
        [[nodiscard]] f32 OffsetOf(f64 frame, f32 width) const noexcept
        {
            return static_cast<f32>((frame - m_first) / m_count * static_cast<f64>(width));
        }

        void Join(IFrameAxisClient* client) { m_clients.PushBack(client); }
        void Leave(IFrameAxisClient* client)
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                if (m_clients[i] == client)
                {
                    m_clients.RemoveAt(i);
                    return;
                }
        }

    private:
        void Show(f64 first, f64 count)
        {
            count = Max(count, 1.0);
            if (m_any)
            {
                const f64 end = m_newest + 1.0;
                first = Clamp(first, m_oldest, Max(m_oldest, end - count));
            }
            if (first == m_first && count == m_count)
                return;
            m_first = first;
            m_count = count;
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnFrameAxisChanged();
        }

        f64 m_first = 0.0;
        f64 m_count = 1.0;
        f64 m_oldest = 0.0;
        f64 m_newest = 0.0;
        bool m_any = false;
        bool m_fits = true;
        Array<IFrameAxisClient*> m_clients; // non-owning: each view leaves as it goes
    };

    /// A column of a strip or a plot: the frames it shows (one, or the several sharing a pixel)
    /// and where it is drawn.
    struct FrameColumn
    {
        u64 first = 0;
        u64 last = 0;
        f32 x = 0.0f;     // from the plot's start
        f32 width = 0.0f;
    };

    /// What a strip and a plot share: the axis, the gutter at their start (the scale, a counter's
    /// name) and the gestures.
    class FrameAxisView : public ProfilerWidget, public IFrameAxisClient
    {
        RTTI_OBJECT(FrameAxisView, ProfilerWidget)
    public:
        /// The gutter at the start: the scale's values, a counter's name. The timeline's lane
        /// names are as wide, so the page's columns line up.
        static constexpr f32 kGutterWidth = 128.0f;
        static constexpr f32 kDragThreshold = 3.0f;

        FrameAxisView() { SetTabFocusable(false); }
        ~FrameAxisView() override
        {
            if (m_axis)
                m_axis->Leave(this);
        }

        void SetFrameAxis(RefPtr<FrameAxis> axis)
        {
            if (m_axis)
                m_axis->Leave(this);
            m_axis = core::Move(axis);
            if (m_axis)
            {
                m_axis->Join(this);
                m_axis->Track(Source());
            }
            Invalidate();
        }
        [[nodiscard]] FrameAxis* GetFrameAxis() const noexcept { return m_axis.Get(); }

        /// Told when the user chooses frames here (a click, a drag): a page stops following then.
        Function<void()> OnFramesPicked;

        /// The plot's bounds (its own space): the content less the gutter.
        [[nodiscard]] Rect PlotBounds() const
        {
            const Rect box = GetContentBounds();
            const f32 gutter = Min(kGutterWidth, box.width);
            return Rect{box.x + gutter, box.y, Max(box.width - gutter, 0.0f), box.height};
        }
        /// The frame at a point's x (its own space), clamped to the source's frames; false off them.
        [[nodiscard]] bool FrameAtX(f32 x, u64& frame) const
        {
            const IProfileSource* source = Source();
            if (!m_axis || source == nullptr || !source->HasFrames())
                return false;
            const Rect plot = PlotBounds();
            const f64 at = m_axis->FrameAt(x - plot.x, plot.width);
            const f64 clamped = Clamp(FrameAxis::Whole(at), static_cast<f64>(source->OldestFrame()), static_cast<f64>(source->NewestFrame()));
            frame = static_cast<u64>(clamped);
            return true;
        }
        /// The columns across the plot now: a frame each, or the frames sharing a pixel.
        void GetColumns(Array<FrameColumn>& out) const
        {
            out.Clear();
            const IProfileSource* source = Source();
            if (!m_axis || source == nullptr || !source->HasFrames())
                return;
            const Rect plot = PlotBounds();
            if (plot.width <= 0.0f)
                return;
            const u64 oldest = source->OldestFrame();
            const u64 newest = source->NewestFrame();
            const f64 perPixel = m_axis->Count() / static_cast<f64>(plot.width);
            if (perPixel <= 1.0)
            {
                const u64 first = static_cast<u64>(Max(FrameAxis::Whole(m_axis->First()), static_cast<f64>(oldest)));
                const f64 end = Min(m_axis->First() + m_axis->Count(), static_cast<f64>(newest) + 1.0);
                for (u64 n = first; static_cast<f64>(n) < end; ++n)
                {
                    FrameColumn column;
                    column.first = column.last = n;
                    column.x = m_axis->OffsetOf(static_cast<f64>(n), plot.width);
                    column.width = m_axis->OffsetOf(static_cast<f64>(n + 1), plot.width) - column.x;
                    out.PushBack(column);
                }
                return;
            }
            const i32 pixels = static_cast<i32>(Ceil(plot.width));
            for (i32 x = 0; x < pixels; ++x)
            {
                const f64 from = Max(FrameAxis::Whole(m_axis->FrameAt(static_cast<f32>(x), plot.width)), static_cast<f64>(oldest));
                const f64 to = Min(FrameAxis::Whole(m_axis->FrameAt(static_cast<f32>(x + 1), plot.width) - 1e-9), static_cast<f64>(newest));
                if (to < from)
                    continue;
                FrameColumn column;
                column.first = static_cast<u64>(from);
                column.last = static_cast<u64>(to);
                column.x = static_cast<f32>(x);
                column.width = 1.0f;
                out.PushBack(column);
            }
        }

        /// The frame under the pointer described (its time, a counter's value), else its own tooltip.
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            if (!m_hoverTip.IsEmpty())
                return m_hoverTip.AsView();
            return UIWidget::GetTooltipText();
        }

        /// The parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            ProfilerWidget::CollectStyleParts(out);
            out.PushBack(StringView(u8"plot"));
            out.PushBack(StringView(u8"chosen-box"));
            out.PushBack(StringView(u8"chosen-fill"));
        }

        void OnFrameAxisChanged() override { Invalidate(); }
        void OnProfileSourceChanged() override
        {
            if (m_axis)
                m_axis->Track(Source());
            Invalidate();
        }

    protected:
        void OnMouseDown(const MouseEvent& event) override
        {
            if (!m_axis)
                return;
            const core::Float2 at = Local(event.Position);
            if (event.Button == MouseButton::Left)
            {
                event.Handled = true;
                if (event.ClickCount == 2)
                {
                    const IProfileSource* source = Source();
                    if (source != nullptr && source->HasFrames())
                        m_axis->Fit(source->OldestFrame(), source->NewestFrame());
                    return;
                }
                if (!FrameAtX(at.x, m_pressFrame))
                    return;
                m_choosing = true;
                m_pressX = at.x;
                Pick(m_pressFrame, m_pressFrame);
            }
            else if (event.Button == MouseButton::Middle)
            {
                event.Handled = true;
                m_panning = true;
                m_panFrom = at.x;
                m_panFirst = m_axis->First();
            }
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            const core::Float2 at = Local(event.Position);
            m_hoverX = at.x;
            m_hovering = true;
            if (m_choosing && Abs(at.x - m_pressX) >= kDragThreshold)
            {
                u64 frame = 0;
                if (FrameAtX(at.x, frame))
                    Pick(Min(frame, m_pressFrame), Max(frame, m_pressFrame));
            }
            else if (m_panning && m_axis)
            {
                const Rect plot = PlotBounds();
                const f64 perPixel = m_axis->Count() / static_cast<f64>(Max(plot.width, 1.0f));
                m_axis->PanBy(m_panFirst - m_axis->First() - static_cast<f64>(at.x - m_panFrom) * perPixel);
            }
            UpdateHoverTip();
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            if (event.Button == MouseButton::Left)
                m_choosing = false;
            if (event.Button == MouseButton::Middle)
                m_panning = false;
        }
        void OnPointerCancel(const MouseEvent&) override
        {
            m_choosing = false;
            m_panning = false;
        }
        void OnMouseLeave(const MouseEvent& event) override
        {
            UIWidget::OnMouseLeave(event);
            m_hovering = false;
            m_hoverTip.Clear();
        }
        void OnMouseWheel(const WheelEvent& event) override
        {
            if (!m_axis || event.Delta.y == 0.0f)
                return;
            event.Handled = true;
            const Rect plot = PlotBounds();
            const core::Float2 at = Local(event.Position);
            m_axis->ZoomAbout(m_axis->FrameAt(at.x - plot.x, plot.width), static_cast<f64>(Pow(1.25f, event.Delta.y)));
        }
        [[nodiscard]] Cursor GetCursorNow() const override { return m_panning ? Cursor::Grabbing : UIWidget::GetCursorNow(); }

        /// What the tooltip says over `frame` (the frame under the pointer).
        [[nodiscard]] virtual String DescribeFrame(u64 frame) const = 0;

        /// The frames chosen, drawn: their columns filled in the accent are the view's own; the
        /// box around them is common.
        void DrawChosenBox(DrawContext& ctx, Span<const FrameColumn> columns, const Rect& plot) const
        {
            const FrameRange chosen = m_focus ? m_focus->Frames() : FrameRange{};
            if (chosen.empty || columns.IsEmpty())
                return;
            f32 from = -1.0f;
            f32 to = -1.0f;
            for (const FrameColumn& column : columns)
                if (column.last >= chosen.first && column.first <= chosen.last)
                {
                    if (from < 0.0f)
                        from = column.x;
                    to = column.x + column.width;
                }
            if (from < 0.0f)
                return;
            const Color edge = PartColor(u8"chosen-box", Color{0.36f, 0.55f, 0.95f, 1.0f});
            const Color fill = PartColor(u8"chosen-fill", Color{0.18f, 0.43f, 0.87f, 0.14f});
            const Rect box{plot.x + from, plot.y, Max(to - from, 2.0f), plot.height};
            ctx.VG().FillRect(box.ToRectangle(), fill);
            ctx.VG().StrokeRect(box.ToRectangle(), edge, 1.0f);
        }

        RefPtr<FrameAxis> m_axis;

    private:
        void Pick(u64 first, u64 last)
        {
            if (!m_focus)
                return;
            FrameRange range;
            range.first = first;
            range.last = last;
            range.empty = false;
            m_focus->SetFrames(range);
            if (OnFramesPicked)
                OnFramesPicked();
        }
        void UpdateHoverTip()
        {
            m_hoverTip.Clear();
            u64 frame = 0;
            if (m_hovering && m_hoverX >= PlotBounds().x && FrameAtX(m_hoverX, frame))
                m_hoverTip = DescribeFrame(frame);
        }

        bool m_choosing = false;
        u64 m_pressFrame = 0;
        f32 m_pressX = 0.0f;
        bool m_panning = false;
        f32 m_panFrom = 0.0f;
        f64 m_panFirst = 0.0;
        bool m_hovering = false;
        f32 m_hoverX = 0.0f;
        String m_hoverTip;
    };

    /// A column's frames: the longest one's CPU and GPU times, and the chosen name's longest
    /// share in them.
    struct FrameColumnTimes
    {
        f32 cpuMs = 0.0f;
        f32 gpuMs = 0.0f;
        f32 chosenMs = 0.0f;
        bool any = false;
        bool chosen = false; // a frame of it is among those chosen
    };

    /// The frames' CPU times against a budget; the frames chosen; the name chosen inside them.
    class FrameStrip final : public FrameAxisView
    {
        RTTI_OBJECT(FrameStrip, FrameAxisView)
    public:
        static constexpr f32 kDefaultHeight = 64.0f;

        FrameStrip()
        {
            SetTag(StringView(u8"framestrip"));
            EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength::Px(kDefaultHeight); });
        }

        /// The frame's budget in ms (a frame within it is on time); 0: none.
        [[nodiscard]] f32 BudgetMs() const noexcept { return m_budgetMs; }
        void SetBudgetMs(f32 ms)
        {
            m_budgetMs = Max(ms, 0.0f);
            Invalidate();
        }

        /// The bars' scale (ms at the plot's top) over these columns: twice the budget, or without
        /// one the longest frame shown rounded up to 1, 2 or 5 times a power of ten.
        [[nodiscard]] f32 ScaleMs(Span<const FrameColumnTimes> times) const noexcept
        {
            if (m_budgetMs > 0.0f)
                return m_budgetMs * 2.0f;
            f32 most = 0.0f;
            for (const FrameColumnTimes& t : times)
                most = Max(most, t.cpuMs);
            return NiceCeiling(Max(most, 1.0f));
        }
        /// Each column's times (the longest of its frames), in the columns' order.
        void GetColumnTimes(Span<const FrameColumn> columns, Array<FrameColumnTimes>& out) const
        {
            out.Clear();
            const IProfileSource* source = Source();
            if (source == nullptr)
                return;
            const u64 frequency = source->TickFrequency();
            const FrameRange chosen = m_focus ? m_focus->Frames() : FrameRange{};
            const ChosenName name = m_focus ? m_focus->Name() : ChosenName{};
            for (const FrameColumn& column : columns)
            {
                FrameColumnTimes times;
                for (u64 n = column.first; n <= column.last; ++n)
                {
                    const FrameRecord* frame = source->Frame(n);
                    if (frame == nullptr)
                        continue;
                    times.any = true;
                    times.cpuMs = Max(times.cpuMs, TicksToMs(frame->endTick - frame->beginTick, frequency));
                    if (name.any)
                        times.chosenMs = Max(times.chosenMs, TicksToMs(NameTicksInFrame(*frame, name), frequency));
                    if (frame->gpuValid && !frame->gpu.IsEmpty())
                    {
                        u64 begin = frame->gpu[0].beginTick;
                        u64 end = frame->gpu[0].endTick;
                        for (const GpuPassEvent& pass : frame->gpu)
                        {
                            begin = Min(begin, pass.beginTick);
                            end = Max(end, pass.endTick);
                        }
                        times.gpuMs = Max(times.gpuMs, TicksToMs(end - begin, frequency));
                    }
                    if (!chosen.empty && n >= chosen.first && n <= chosen.last)
                        times.chosen = true;
                }
                out.PushBack(times);
            }
        }

        /// The parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            FrameAxisView::CollectStyleParts(out);
            out.PushBack(StringView(u8"scale"));
            out.PushBack(StringView(u8"on-time"));
            out.PushBack(StringView(u8"over"));
            out.PushBack(StringView(u8"far-over"));
            out.PushBack(StringView(u8"clipped"));
            out.PushBack(StringView(u8"chosen"));
            out.PushBack(StringView(u8"share"));
            out.PushBack(StringView(u8"budget"));
        }

        void OnProfileChoiceChanged() override { Invalidate(); }

    protected:
        String DescribeFrame(u64 frame) const override
        {
            const IProfileSource* source = Source();
            const FrameRecord* record = source != nullptr ? source->Frame(frame) : nullptr;
            if (record == nullptr)
                return Format(u8"Frame {}: not recorded", frame);
            const u64 frequency = source->TickFrequency();
            String text = Format(u8"Frame {}: {} ms", frame, FormatFixed(TicksToMs(record->endTick - record->beginTick, frequency), 2).AsView());
            if (m_focus && m_focus->Name().any)
                AppendFormat(text, u8", {} {} ms", source->NameOf(m_focus->Name().name),
                             FormatFixed(TicksToMs(NameTicksInFrame(*record, m_focus->Name()), frequency), 2).AsView());
            return text;
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            const Rect plot = PlotBounds();
            Array<FrameColumn> columns;
            GetColumns(columns);
            Array<FrameColumnTimes> times;
            GetColumnTimes(columns.AsSpan(), times);
            const f32 scale = ScaleMs(times.AsSpan());
            const Color plotColor = PartColor(u8"plot", Color{0.0f, 0.0f, 0.0f, 0.18f});
            ctx.VG().FillRect(plot.ToRectangle(), plotColor);

            // The scale: its top, the budget's line where there is one, its foot.
            const Color dim = PartTextColor(u8"scale");
            const Rect gutter{plot.x - kGutterWidth, plot.y, kGutterWidth - 6.0f, plot.height};
            DrawLabel(ctx, Format(u8"{}", FormatFixed(scale, 1).AsView()).AsView(), Rect{gutter.x, plot.y, gutter.width, 12.0f}, dim, TextHAlign::End);
            DrawLabel(ctx, StringView(u8"0 ms"), Rect{gutter.x, plot.Bottom() - 12.0f, gutter.width, 12.0f}, dim, TextHAlign::End);

            const Color onTime = PartColor(u8"on-time", Color{0.25f, 0.61f, 0.42f, 1.0f});
            const Color over = PartColor(u8"over", Color{0.79f, 0.60f, 0.18f, 1.0f});
            const Color far = PartColor(u8"far-over", Color{0.78f, 0.31f, 0.25f, 1.0f});
            const Color chosenColor = PartColor(u8"chosen", Color{0.18f, 0.43f, 0.87f, 1.0f});
            const Color share = PartColor(u8"share", Color{0.0f, 0.0f, 0.0f, 0.40f});
            const Color clip = PartColor(u8"clipped", far);
            ctx.PushClip(plot);
            for (usize i = 0; i < columns.Size(); ++i)
            {
                const FrameColumnTimes& t = times[i];
                if (!t.any)
                    continue;
                const FrameColumn& column = columns[i];
                const f32 inset = column.width >= 3.0f ? column.width * 0.12f : 0.0f;
                const f32 x = plot.x + column.x + inset;
                const f32 w = Max(column.width - inset * 2.0f, 1.0f);
                const f32 h = Min(t.cpuMs / scale, 1.0f) * plot.height;
                Color color = onTime;
                if (m_budgetMs > 0.0f && t.cpuMs > m_budgetMs * 2.0f)
                    color = far;
                else if (m_budgetMs > 0.0f && t.cpuMs > m_budgetMs)
                    color = over;
                if (t.chosen)
                    color = chosenColor;
                ctx.VG().FillRect(Rectangle{x, plot.Bottom() - h, w, h}, color);
                if (t.chosenMs > 0.0f)
                {
                    const f32 sh = Min(t.chosenMs / scale, 1.0f) * plot.height;
                    ctx.VG().FillRect(Rectangle{x, plot.Bottom() - sh, w, sh}, share);
                }
                if (t.cpuMs > scale)
                    ctx.VG().FillRect(Rectangle{x, plot.y, w, 3.0f}, clip);
            }
            if (m_budgetMs > 0.0f)
            {
                const Color line = PartColor(u8"budget", Color{0.6f, 0.6f, 0.6f, 0.9f});
                const f32 y = Floor(plot.Bottom() - m_budgetMs / scale * plot.height);
                ctx.VG().FillRect(Rectangle{plot.x, y, plot.width, 1.0f}, line);
                ctx.PopClip();
                DrawLabel(ctx, FormatFixed(m_budgetMs, 1).AsView(), Rect{gutter.x, y - 6.0f, gutter.width, 12.0f}, dim, TextHAlign::End);
                ctx.PushClip(plot);
            }
            DrawChosenBox(ctx, columns.AsSpan(), plot);
            ctx.PopClip();
        }

    private:
        // The least of 1, 2 or 5 times a power of ten at least `value`.
        [[nodiscard]] static f32 NiceCeiling(f32 value) noexcept
        {
            const f32 magnitude = Pow(10.0f, Floor(Log(value) / Log(10.0f)));
            const f32 multiples[] = {1.0f, 2.0f, 5.0f, 10.0f};
            for (const f32 m : multiples)
                if (m * magnitude >= value * 0.9999f)
                    return m * magnitude;
            return 10.0f * magnitude;
        }

        f32 m_budgetMs = 0.0f;
    };

    /// A counter's value each frame, on the strip's axis: its name in the gutter, the least and
    /// the most shown printed over the plot.
    class CounterPlot final : public FrameAxisView
    {
        RTTI_OBJECT(CounterPlot, FrameAxisView)
    public:
        static constexpr f32 kDefaultHeight = 26.0f;

        CounterPlot()
        {
            SetTag(StringView(u8"counterplot"));
            EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength::Px(kDefaultHeight); });
        }

        /// The counter it plots, by its index in the source's counters.
        [[nodiscard]] u32 Counter() const noexcept { return m_counter; }
        void SetCounter(u32 counter)
        {
            m_counter = counter;
            Invalidate();
        }

        /// The parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            FrameAxisView::CollectStyleParts(out);
            out.PushBack(StringView(u8"name"));
            out.PushBack(StringView(u8"bar"));
            out.PushBack(StringView(u8"chosen"));
            out.PushBack(StringView(u8"range"));
            out.PushBack(StringView(u8"range-backing"));
        }

        /// A column's value: the most of its frames' (false when none of them set it).
        [[nodiscard]] bool ColumnValue(const FrameColumn& column, f64& value) const
        {
            const IProfileSource* source = Source();
            bool any = false;
            for (u64 n = column.first; source != nullptr && n <= column.last; ++n)
                if (const FrameRecord* frame = source->Frame(n))
                    for (const CounterValue& v : frame->counters)
                        if (v.counter == m_counter)
                        {
                            value = any ? Max(value, v.value) : v.value;
                            any = true;
                        }
            return any;
        }

    protected:
        String DescribeFrame(u64 frame) const override
        {
            const IProfileSource* source = Source();
            if (source == nullptr || m_counter >= source->Counters().Size())
                return String();
            const CounterInfo& info = source->Counters()[m_counter];
            FrameColumn column;
            column.first = column.last = frame;
            f64 value = 0.0;
            if (!ColumnValue(column, value))
                return Format(u8"{} at frame {}: not set", info.name.AsView(), frame);
            return Format(u8"{} at frame {}: {}", info.name.AsView(), frame, FormatCounter(value, info.unit, MemoryAllocator()).AsView());
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            const Rect plot = PlotBounds();
            ctx.VG().FillRect(plot.ToRectangle(), PartColor(u8"plot", Color{0.0f, 0.0f, 0.0f, 0.18f}));
            const IProfileSource* source = Source();
            if (source == nullptr || m_counter >= source->Counters().Size())
                return;
            const CounterInfo& info = source->Counters()[m_counter];
            DrawLabel(ctx, info.name.AsView(), Rect{plot.x - kGutterWidth, plot.y, kGutterWidth - 6.0f, plot.height},
                      PartTextColor(u8"name"), TextHAlign::End);

            Array<FrameColumn> columns;
            GetColumns(columns);
            Array<f64> values;
            Array<u8> set;
            f64 least = 0.0;
            f64 most = 0.0;
            bool any = false;
            for (const FrameColumn& column : columns)
            {
                f64 value = 0.0;
                const bool has = ColumnValue(column, value);
                values.PushBack(value);
                set.PushBack(has ? 1 : 0);
                if (has)
                {
                    least = any ? Min(least, value) : value;
                    most = any ? Max(most, value) : value;
                    any = true;
                }
            }
            if (!any)
                return;
            const Color bar = PartColor(u8"bar", Color{0.44f, 0.53f, 0.72f, 1.0f});
            const Color chosenColor = PartColor(u8"chosen", Color{0.18f, 0.43f, 0.87f, 1.0f});
            const FrameRange chosen = m_focus ? m_focus->Frames() : FrameRange{};
            const f64 span = Max(most - least, 1e-9);
            ctx.PushClip(plot);
            for (usize i = 0; i < columns.Size(); ++i)
            {
                if (set[i] == 0)
                    continue;
                const FrameColumn& column = columns[i];
                // The least shown stands 2 pixels high, so a flat counter still shows.
                const f32 h = 2.0f + static_cast<f32>((values[i] - least) / span) * (plot.height - 4.0f);
                const bool isChosen = !chosen.empty && column.last >= chosen.first && column.first <= chosen.last;
                ctx.VG().FillRect(Rectangle{plot.x + column.x, plot.Bottom() - h, Max(column.width, 1.0f), h}, isChosen ? chosenColor : bar);
            }
            ctx.PopClip();
            const String range = Format(u8"{} to {}", FormatCounter(least, info.unit, MemoryAllocator()).AsView(),
                                        FormatCounter(most, info.unit, MemoryAllocator()).AsView());
            // On a backing of the plot's colour at its end, so the bars under it never hide it.
            const f32 width = Min(LabelWidth(range.AsView()) + 8.0f, plot.width);
            const Rect backing{plot.Right() - width, plot.y + 2.0f, width, plot.height - 4.0f};
            ctx.VG().FillRoundedRect(backing.ToRectangle(), 3.0f, PartColor(u8"range-backing", Color{0.1f, 0.1f, 0.12f, 0.85f}));
            DrawLabel(ctx, range.AsView(), Rect{backing.x + 4.0f, plot.y, width - 8.0f, plot.height}, PartTextColor(u8"range"), TextHAlign::End);
        }

    private:
        u32 m_counter = 0;
    };
}
