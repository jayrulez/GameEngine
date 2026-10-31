// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Profiler.GUI - :frame_strip partition
//
// Frames side by side: the frame strip, and the counter plots under it. They share one FrameAxis,
// so they show the same frames at the same positions.
//
// Each bar in the strip is one frame's CPU time against a budget, coloured by whether the frame is
// within the budget, over it, or over twice it. The scale prints its values; it is twice the
// budget, or without a budget the longest frame shown. A bar past the scale is clipped and marked.
// The chosen frames are drawn in the accent colour and boxed. The chosen name (a scope or a pass)
// is drawn inside each bar as its share of the frame. A counter plot shows the counter's value in
// each frame between the least and the most shown, and prints that range. When there are more
// frames than pixels, the frames share a column that shows the longest of them, so a hitch is
// never hidden by its neighbours.
//
// The mouse works the same on all of them: a click chooses a frame and a drag chooses a range,
// the wheel zooms about the pointer, a middle drag pans, and a double click shows every frame
// again. While the axis fits (until a zoom or a pan) it follows the source's frames as they grow.

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
    /// The notifications a frame axis sends to the views on it.
    class IFrameAxisClient
    {
    public:
        virtual ~IFrameAxisClient() = default;
        virtual void OnFrameAxisChanged() = 0;
    };

    /// FrameAxis::kMinFrames, at namespace scope so another library can read it: MSVC does not
    /// export a class's static constexpr member from a shared library.
    inline constexpr f64 kFrameAxisMinFrames = 8.0;

    /// The frames a strip and its plots show: the frame at their start, and how many lie across.
    class FrameAxis : public RefCounted
    {
    public:
        /// The fewest frames shown across; zooming in stops here.
        static constexpr f64 kMinFrames = kFrameAxisMinFrames;

        explicit FrameAxis(IAllocator& allocator) : m_clients(allocator) {}

        /// Returns the frame number at the views' start. It is fractional, since a pan can move by
        /// less than a frame.
        [[nodiscard]] f64 First() const noexcept { return m_first; }
        /// Returns how many frames lie across the views.
        [[nodiscard]] f64 Count() const noexcept { return m_count; }
        /// Returns whether the axis shows every frame of the source, following the frames as they grow.
        [[nodiscard]] bool Fits() const noexcept { return m_fits; }

        /// Shows [oldest, newest], and keeps showing every frame as Track reports more.
        void Fit(u64 oldest, u64 newest)
        {
            m_fits = true;
            Show(static_cast<f64>(oldest), static_cast<f64>(Max(newest, oldest) - oldest + 1));
        }
        /// Updates the axis with the source's current frames. While the axis fits, it shows them all.
        void Track(const IProfileSource* source)
        {
            m_oldest = source != nullptr && source->HasFrames() ? static_cast<f64>(source->OldestFrame()) : 0.0;
            m_newest = source != nullptr && source->HasFrames() ? static_cast<f64>(source->NewestFrame()) : 0.0;
            m_any = source != nullptr && source->HasFrames();
            if (m_fits)
                Show(m_oldest, m_any ? m_newest - m_oldest + 1.0 : 1.0);
        }
        /// Zooms by `factor` (above 1 zooms in) about `frame`, which stays in place. It shows no
        /// fewer than kMinFrames across and no more than every frame.
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
        /// Pans by `frames` (positive shows later frames), staying within the source's frames.
        void PanBy(f64 frames)
        {
            m_fits = false;
            Show(m_first + frames, m_count);
        }

        /// Returns the whole frame number at or below `frame`. This uses f64 because frame numbers go
        /// far past the range of whole numbers f32 represents exactly.
        [[nodiscard]] static f64 Whole(f64 frame) noexcept
        {
            const f64 truncated = static_cast<f64>(static_cast<i64>(frame));
            return truncated > frame ? truncated - 1.0 : truncated;
        }

        /// Returns the frame at `offset` pixels into a view `width` wide; OffsetOf returns where a
        /// frame starts.
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
        Array<IFrameAxisClient*> m_clients; // non-owning; each view leaves when it is destroyed
    };

    /// A column of a strip or a plot: the frames it shows (one, or several sharing a pixel) and
    /// where it is drawn.
    struct FrameColumn
    {
        u64 first = 0;
        u64 last = 0;
        f32 x = 0.0f;     // offset from the plot's start
        f32 width = 0.0f;
    };

    /// The base a strip and a plot share: the axis, the gutter at their start (for the scale or a
    /// counter's name) and the mouse gestures.
    class FrameAxisView : public ProfilerWidget, public IFrameAxisClient
    {
        RTTI_OBJECT(FrameAxisView, ProfilerWidget)
    public:
        /// The gutter's width at the start, which holds the scale's values or a counter's name. The
        /// timeline's lane names are the same width, so the page's columns line up.
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

        /// Called when the user chooses frames here with a click or a drag, so a page can stop
        /// following new frames.
        Function<void()> OnFramesPicked;

        /// Returns the plot's bounds in the view's own space: the content box less the gutter.
        [[nodiscard]] Rect PlotBounds() const
        {
            const Rect box = GetContentBounds();
            const f32 gutter = Min(kGutterWidth, box.width);
            return Rect{box.x + gutter, box.y, Max(box.width - gutter, 0.0f), box.height};
        }
        /// Gets the frame at x (in the view's own space), clamped to the source's frames. Returns
        /// false when there is no axis or no frames.
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
        /// Gets the columns across the plot now: one per frame, or one per pixel when frames share
        /// pixels.
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

        /// Returns a description of the frame under the pointer (its time, or a counter's value), or
        /// else the widget's own tooltip.
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            if (!m_hoverTip.IsEmpty())
                return m_hoverTip.AsView();
            return UIWidget::GetTooltipText();
        }

        /// Lists the parts it draws, which the sheet styles as `tag::part`.
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

        /// Returns the tooltip text for `frame`, the frame under the pointer.
        [[nodiscard]] virtual String DescribeFrame(u64 frame) const = 0;

        /// Draws the box around the chosen frames. Each view fills the chosen columns in the accent
        /// itself; this box is common to them.
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

    /// The times for one column's frames: the longest CPU and GPU times, and the chosen name's
    /// longest time among them.
    struct FrameColumnTimes
    {
        f32 cpuMs = 0.0f;
        f32 gpuMs = 0.0f;
        f32 chosenMs = 0.0f;
        bool any = false;
        bool chosen = false; // one of the column's frames is among those chosen
    };

    /// Shows the frames' CPU times against a budget, the chosen frames, and the chosen name inside
    /// them.
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

        /// Returns the frame budget in ms; a frame within it is on time. 0 means no budget.
        [[nodiscard]] f32 BudgetMs() const noexcept { return m_budgetMs; }
        void SetBudgetMs(f32 ms)
        {
            m_budgetMs = Max(ms, 0.0f);
            Invalidate();
        }

        /// Returns the bars' scale (the ms at the plot's top) for these columns: twice the budget,
        /// or without a budget the longest frame shown, rounded up to 1, 2 or 5 times a power of ten.
        [[nodiscard]] f32 ScaleMs(Span<const FrameColumnTimes> times) const noexcept
        {
            if (m_budgetMs > 0.0f)
                return m_budgetMs * 2.0f;
            f32 most = 0.0f;
            for (const FrameColumnTimes& t : times)
                most = Max(most, t.cpuMs);
            return NiceCeiling(Max(most, 1.0f));
        }
        /// Gets each column's times (from the longest of its frames), in the columns' order.
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

        /// Lists the parts it draws, which the sheet styles as `tag::part`.
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

            // Draw the scale: its top value, the budget line when there is a budget, and its foot.
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
        // Returns the smallest value of the form 1, 2 or 5 times a power of ten that is at least
        // `value`.
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

    /// A counter's value in each frame, on the strip's axis. The counter's name is in the gutter,
    /// and the least and most values shown, with the mean over the chosen frames, are printed over
    /// the plot.
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

        /// Returns the plotted counter's index in the source's counters.
        [[nodiscard]] u32 Counter() const noexcept { return m_counter; }
        void SetCounter(u32 counter)
        {
            m_counter = counter;
            Invalidate();
        }

        /// Lists the parts it draws, which the sheet styles as `tag::part`.
        void CollectStyleParts(Array<StringView>& out) const override
        {
            FrameAxisView::CollectStyleParts(out);
            out.PushBack(StringView(u8"name"));
            out.PushBack(StringView(u8"bar"));
            out.PushBack(StringView(u8"chosen"));
            out.PushBack(StringView(u8"range"));
            out.PushBack(StringView(u8"range-backing"));
        }

        /// Gets a column's value, the largest among its frames. Returns false when none of them set it.
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

        /// Gets the counter's mean over the chosen frames that set it. Returns false when no frames
        /// are chosen or none of them set it.
        [[nodiscard]] bool ChosenMean(f64& mean) const
        {
            const IProfileSource* source = Source();
            const FrameRange chosen = m_focus ? m_focus->Frames() : FrameRange{};
            if (source == nullptr || chosen.empty)
                return false;
            f64 sum = 0.0;
            u32 count = 0;
            for (u64 n = chosen.first; n <= chosen.last; ++n)
                if (const FrameRecord* frame = source->Frame(n))
                    for (const CounterValue& v : frame->counters)
                        if (v.counter == m_counter)
                        {
                            sum += v.value;
                            ++count;
                        }
            if (count == 0)
                return false;
            mean = sum / static_cast<f64>(count);
            return true;
        }
        /// Returns the text printed over the plot: the least and most values shown, and the chosen
        /// frames' mean.
        [[nodiscard]] String RangeText(f64 least, f64 most) const
        {
            const IProfileSource* source = Source();
            if (source == nullptr || m_counter >= source->Counters().Size())
                return String();
            const CounterUnit unit = source->Counters()[m_counter].unit;
            String text = Format(u8"{} to {}", FormatCounter(least, unit, MemoryAllocator()).AsView(), FormatCounter(most, unit, MemoryAllocator()).AsView());
            f64 mean = 0.0;
            if (ChosenMean(mean))
                text += Format(u8", mean {}", FormatCounter(mean, unit, MemoryAllocator()).AsView()).AsView();
            return text;
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
                // The least value shown is drawn 2 pixels high, so a flat counter is still visible.
                const f32 h = 2.0f + static_cast<f32>((values[i] - least) / span) * (plot.height - 4.0f);
                const bool isChosen = !chosen.empty && column.last >= chosen.first && column.first <= chosen.last;
                ctx.VG().FillRect(Rectangle{plot.x + column.x, plot.Bottom() - h, Max(column.width, 1.0f), h}, isChosen ? chosenColor : bar);
            }
            ctx.PopClip();
            const String range = RangeText(least, most);
            // Draw the text at the plot's end on a backing of the plot's colour, so the bars under
            // it never hide it.
            const f32 width = Min(LabelWidth(range.AsView()) + 8.0f, plot.width);
            const Rect backing{plot.Right() - width, plot.y + 2.0f, width, plot.height - 4.0f};
            ctx.VG().FillRoundedRect(backing.ToRectangle(), 3.0f, PartColor(u8"range-backing", Color{0.1f, 0.1f, 0.12f, 0.85f}));
            DrawLabel(ctx, range.AsView(), Rect{backing.x + 4.0f, plot.y, width - 8.0f, plot.height}, PartTextColor(u8"range"), TextHAlign::End);
        }

    private:
        u32 m_counter = 0;
    };
}
