// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Toolkit - :time_axis partition
//
// One time axis (T5 of Specs/gui-lab-evolution-3.md): the scale (pixels per second), the scroll (the
// time at a view's start), a frame rate (labels and snapping in frames) and the playhead, held once
// and read by every view on it (the ruler, a curve view, a dopesheet), none keeping a copy, so views
// stacked on one axis stay in step (foundation.ui's lesson: its curve canvas copied the timeline's).
// The views hold the axis, so it outlives them, and each leaves it as it goes.
//
// TimeRuler draws the axis: ticks at a step picked so labels never collide (the smallest of 1, 2 or 5
// times a power of ten, of seconds or of frames), labels, and the playhead. A drag on it scrubs the
// playhead, the wheel zooms about the pointer and a middle drag pans. Time runs left to right in
// either direction of the text, as a ruler's numbers do.
//
// A ruler may show a marker source's markers (a clip's events), each a time and a label, as a flag at
// its time: a click chooses one, a drag moves it, one gesture through the source (begun, previewed,
// committed once, Escape giving it up), and the one under the pointer names itself in full. The undo
// is the source's, as every source's is.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui.toolkit:time_axis;

import foundation.core;
import foundation.vg;
import foundation.gui;

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    /// `value` to the decimals of `step` (0.25 to two, 5 to none): a ruler's or a grid's label.
    [[nodiscard]] inline core::String FormatStepped(f32 value, f32 step)
    {
        i32 decimals = 0;
        for (f32 s = step; s < 0.999f && decimals < 6; s *= 10.0f)
            ++decimals;
        f64 scale = 1.0;
        for (i32 d = 0; d < decimals; ++d)
            scale *= 10.0;
        const f64 rounded = static_cast<f64>(static_cast<i64>(static_cast<f64>(value) * scale + (value < 0.0f ? -0.5 : 0.5))) / scale;
        if (decimals == 0)
            return core::Format(u8"{}", static_cast<i64>(rounded));
        return core::Format(u8"{}", rounded);
    }

    /// What a time axis tells the views on it.
    class ITimeAxisClient
    {
    public:
        virtual ~ITimeAxisClient() = default;
        /// The scale, the scroll, the frame rate, the duration or the playhead changed: draw again.
        virtual void OnTimeAxisChanged() = 0;
    };

    class TimeAxis : public RefCounted
    {
    public:
        static constexpr f32 kMinPixelsPerSecond = 1.0f;
        static constexpr f32 kMaxPixelsPerSecond = 20000.0f;

        // === The scale and the scroll ===
        [[nodiscard]] f32 PixelsPerSecond() const noexcept { return m_pixelsPerSecond; }
        void SetPixelsPerSecond(f32 pixels)
        {
            pixels = core::Clamp(pixels, kMinPixelsPerSecond, kMaxPixelsPerSecond);
            if (pixels == m_pixelsPerSecond)
                return;
            m_pixelsPerSecond = pixels;
            Changed();
        }
        /// The time at a view's start.
        [[nodiscard]] f32 Scroll() const noexcept { return m_scroll; }
        void SetScroll(f32 seconds)
        {
            seconds = ClampScroll(seconds);
            if (seconds == m_scroll)
                return;
            m_scroll = seconds;
            Changed();
        }
        /// Scales by `factor` keeping `time` where it is in the view (the wheel's zoom about the pointer).
        void ZoomAbout(f32 time, f32 factor)
        {
            const f32 offset = TimeToOffset(time);
            const f32 pixels = core::Clamp(m_pixelsPerSecond * factor, kMinPixelsPerSecond, kMaxPixelsPerSecond);
            const f32 scroll = ClampScroll(time - offset / pixels);
            if (pixels == m_pixelsPerSecond && scroll == m_scroll)
                return;
            m_pixelsPerSecond = pixels;
            m_scroll = scroll;
            Changed();
        }
        /// Fits [start, end] into `width` pixels of a view.
        void Frame(f32 start, f32 end, f32 width)
        {
            if (width <= 0.0f)
                return;
            const f32 span = core::Max(end - start, 1.0f / kMaxPixelsPerSecond * width);
            m_pixelsPerSecond = core::Clamp(width / span, kMinPixelsPerSecond, kMaxPixelsPerSecond);
            m_scroll = ClampScroll(start);
            Changed();
        }

        /// Pixels from a view's start to `time`, and back.
        [[nodiscard]] f32 TimeToOffset(f32 time) const noexcept { return (time - m_scroll) * m_pixelsPerSecond; }
        [[nodiscard]] f32 OffsetToTime(f32 offset) const noexcept { return m_scroll + offset / m_pixelsPerSecond; }

        // === The clip ===
        /// How long what it shows lasts (seconds): where the scroll and the playhead stop.
        [[nodiscard]] f32 Duration() const noexcept { return m_duration; }
        void SetDuration(f32 seconds)
        {
            seconds = core::Max(seconds, 0.0f);
            if (seconds == m_duration)
                return;
            m_duration = seconds;
            m_scroll = ClampScroll(m_scroll);
            m_playhead = core::Clamp(m_playhead, 0.0f, m_duration);
            Changed();
        }
        /// Frames per second, for labels and snapping in frames; 0: seconds, no snapping.
        [[nodiscard]] f32 FrameRate() const noexcept { return m_frameRate; }
        void SetFrameRate(f32 framesPerSecond)
        {
            framesPerSecond = core::Max(framesPerSecond, 0.0f);
            if (framesPerSecond == m_frameRate)
                return;
            m_frameRate = framesPerSecond;
            Changed();
        }
        /// `time` on the nearest frame, with a frame rate; else as it is.
        [[nodiscard]] f32 Snap(f32 time) const noexcept
        {
            if (m_frameRate <= 0.0f)
                return time;
            return core::Round(time * m_frameRate) / m_frameRate;
        }

        // === The playhead ===
        [[nodiscard]] f32 Playhead() const noexcept { return m_playhead; }
        /// Moves the playhead (kept in the clip), telling whoever plays it.
        void SetPlayhead(f32 seconds)
        {
            seconds = core::Clamp(seconds, 0.0f, m_duration);
            if (seconds == m_playhead)
                return;
            m_playhead = seconds;
            if (m_onPlayhead)
                m_onPlayhead(m_playhead);
            Changed();
        }
        /// The playhead moved (a scrub, or code): what samples the clip there.
        void SetOnPlayheadChanged(core::Function<void(f32)> callback) { m_onPlayhead = core::Move(callback); }

        // === Ticks ===
        /// The step between labelled ticks (seconds): of 1, 2 or 5 times a power of ten, of seconds,
        /// or of frames with a frame rate (one frame the least), the smallest at least `minPixels`
        /// wide.
        [[nodiscard]] f32 TickStep(f32 minPixels) const noexcept
        {
            const f32 least = minPixels / m_pixelsPerSecond; // seconds
            if (m_frameRate > 0.0f)
                return PickStep(core::Max(least * m_frameRate, 1.0f)) / m_frameRate;
            return PickStep(least);
        }
        /// `time` as a label: seconds to the step's decimals, or a frame number with a frame rate.
        [[nodiscard]] core::String FormatTime(f32 time, f32 step) const
        {
            if (m_frameRate > 0.0f)
                return core::Format(u8"{}", static_cast<i64>(core::Round(time * m_frameRate)));
            return FormatStepped(time, step);
        }

        // === Views ===
        void Join(ITimeAxisClient* client) { m_clients.PushBack(client); }
        void Leave(ITimeAxisClient* client)
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
        // The smallest of 1, 2, 5 times a power of ten at least `least`.
        [[nodiscard]] static f32 PickStep(f32 least) noexcept
        {
            if (least <= 0.0f)
                return 1.0f;
            const f32 magnitude = core::Pow(10.0f, core::Floor((core::Log(least) / core::Log(10.0f))));
            const f32 multiples[] = {1.0f, 2.0f, 5.0f};
            for (const f32 m : multiples)
                if (m * magnitude >= least * 0.9999f)
                    return m * magnitude;
            return 10.0f * magnitude;
        }
        [[nodiscard]] f32 ClampScroll(f32 seconds) const noexcept { return core::Clamp(seconds, 0.0f, m_duration); }
        void Changed()
        {
            for (usize i = 0; i < m_clients.Size(); ++i)
                m_clients[i]->OnTimeAxisChanged();
        }

        f32 m_pixelsPerSecond = 100.0f;
        f32 m_scroll = 0.0f;
        f32 m_duration = 10.0f;
        f32 m_frameRate = 0.0f;
        f32 m_playhead = 0.0f;
        core::Array<ITimeAxisClient*> m_clients; // non-owning: each leaves as it goes
        core::Function<void(f32)> m_onPlayhead;
    };

    /// A labelled tick of a ruler.
    struct TimeTick
    {
        f32 Time = 0.0f;
        f32 Offset = 0.0f; ///< from the ruler's content start
        bool Major = false; ///< labelled
        core::String Label;
    };

    /// A marker of a ruler (a clip's event): where it is and what it is called.
    struct TimeMarker
    {
        f32 Time = 0.0f;
        core::String Label;
    };

    /// What a marker source tells the rulers showing it.
    class ITimeMarkerSourceClient
    {
    public:
        virtual ~ITimeMarkerSourceClient() = default;
        /// The markers changed: read them again.
        virtual void OnTimeMarkersChanged() = 0;
    };

    /// The markers a ruler shows, by their place in it; a move is a gesture as the other sources' are.
    class ITimeMarkerSource
    {
    public:
        virtual ~ITimeMarkerSource() = default;

        [[nodiscard]] virtual usize MarkerCount() const = 0;
        [[nodiscard]] virtual TimeMarker GetMarker(usize marker) const = 0;

        /// A move begins: where the marker is now is what a cancel puts back.
        virtual void BeginMarkerMove(usize marker) = 0;
        /// The marker moved to `time`, live.
        virtual void PreviewMarkerMove(usize marker, f32 time) = 0;
        /// The move ends as last previewed: one change.
        virtual void CommitMarkerMove() = 0;
        /// The move is given up: the marker as it was.
        virtual void CancelMarkerMove() = 0;

        void AddMarkerClient(ITimeMarkerSourceClient* client) { m_markerClients.PushBack(client); }
        void RemoveMarkerClient(ITimeMarkerSourceClient* client)
        {
            for (usize i = 0; i < m_markerClients.Size(); ++i)
                if (m_markerClients[i] == client)
                {
                    m_markerClients.RemoveAt(i);
                    return;
                }
        }

    protected:
        void NotifyTimeMarkersChanged()
        {
            for (usize i = 0; i < m_markerClients.Size(); ++i)
                m_markerClients[i]->OnTimeMarkersChanged();
        }

    private:
        core::Array<ITimeMarkerSourceClient*> m_markerClients; // non-owning: each ruler leaves as it goes
    };

    /// The axis drawn: ticks, labels and the playhead; a drag scrubs, the wheel zooms, a middle drag pans.
    class TimeRuler : public UIWidget, public ITimeAxisClient, public ITimeMarkerSourceClient
    {
        RTTI_OBJECT(TimeRuler, UIWidget)
    public:
        /// Labelled ticks at least this far apart (pixels).
        static constexpr f32 kMinLabelSpacing = 60.0f;
        /// A marker is under the pointer this near its time (pixels).
        static constexpr f32 kMarkerHitRadius = 5.0f;
        static constexpr f32 kDragThreshold = 3.0f;

        TimeRuler()
        {
            SetTag(core::StringView(u8"timeruler"));
            SetTabFocusable(true); // Escape reaches it during a marker's move
            EditLayoutStyle([](LayoutStyle& s) { s.Height = LayoutLength::Px(22.0f); });
        }
        ~TimeRuler() override
        {
            if (m_axis)
                m_axis->Leave(this);
            if (m_markers)
                m_markers->RemoveMarkerClient(this);
        }

        void SetTimeAxis(core::RefPtr<TimeAxis> axis)
        {
            if (m_axis)
                m_axis->Leave(this);
            m_axis = core::Move(axis);
            if (m_axis)
                m_axis->Join(this);
            Invalidate();
        }
        [[nodiscard]] TimeAxis* GetTimeAxis() const noexcept { return m_axis.Get(); }

        // === Markers ===
        /// The markers it shows (not owned: the source outlives the ruler or is set to null first).
        void SetMarkerSource(ITimeMarkerSource* source)
        {
            if (m_markers)
                m_markers->RemoveMarkerClient(this);
            m_markers = source;
            if (m_markers)
                m_markers->AddMarkerClient(this);
            m_chosen = -1;
            m_hover = -1;
            m_hoverTip.Clear();
            Invalidate();
        }
        [[nodiscard]] ITimeMarkerSource* GetMarkerSource() const noexcept { return m_markers; }
        /// The marker chosen (-1: none).
        [[nodiscard]] i32 GetChosenMarker() const noexcept { return m_chosen; }
        /// Chooses `marker` (-1: none), as a click on it does, telling OnMarkerChosen.
        void ChooseMarker(i32 marker)
        {
            if (!m_markers || marker < 0 || static_cast<usize>(marker) >= m_markers->MarkerCount())
                marker = -1;
            if (marker == m_chosen)
                return;
            m_chosen = marker;
            Invalidate();
            if (OnMarkerChosen)
                OnMarkerChosen(m_chosen);
        }
        /// Told when the chosen marker changes (-1: none).
        core::Function<void(i32)> OnMarkerChosen;
        /// The marker at `x` from its content start, the nearest within the hit radius; -1: none.
        [[nodiscard]] i32 MarkerAt(f32 x) const
        {
            if (!m_markers || !m_axis)
                return -1;
            i32 best = -1;
            f32 nearest = kMarkerHitRadius;
            for (usize i = 0; i < m_markers->MarkerCount(); ++i)
            {
                const f32 off = core::Abs(m_axis->TimeToOffset(m_markers->GetMarker(i).Time) - x);
                if (off <= nearest)
                {
                    nearest = off;
                    best = static_cast<i32>(i);
                }
            }
            return best;
        }

        void OnTimeMarkersChanged() override
        {
            if (m_markers && m_chosen >= 0 && static_cast<usize>(m_chosen) >= m_markers->MarkerCount())
                ChooseMarker(-1);
            Invalidate();
        }
        /// The marker under the pointer named in full, else its own tooltip.
        [[nodiscard]] core::StringView GetTooltipText() const override
        {
            if (!m_hoverTip.IsEmpty())
                return m_hoverTip.AsView();
            return UIWidget::GetTooltipText();
        }

        /// The ticks across its width now: every minor tick, the major ones labelled.
        void GetTicks(core::Array<TimeTick>& out) const
        {
            out.Clear();
            if (!m_axis)
                return;
            const Rect box = GetContentBounds();
            const f32 step = m_axis->TickStep(kMinLabelSpacing);
            const f32 minor = step / MinorDivisions(step);
            const f32 end = m_axis->OffsetToTime(box.width);
            const i64 first = static_cast<i64>(core::Ceil(m_axis->Scroll() / minor - 0.001f));
            for (i64 i = first;; ++i)
            {
                const f32 time = static_cast<f32>(i) * minor;
                if (time > end + minor * 0.001f || time > m_axis->Duration() + minor * 0.001f)
                    break;
                TimeTick tick;
                tick.Time = time;
                tick.Offset = m_axis->TimeToOffset(time);
                const f32 steps = time / step;
                tick.Major = core::Abs(steps - core::Round(steps)) < 0.001f;
                if (tick.Major)
                    tick.Label = m_axis->FormatTime(time, step);
                out.PushBack(core::Move(tick));
            }
        }

        void OnTimeAxisChanged() override { Invalidate(); }

    protected:
        void OnMouseDown(const MouseEvent& event) override
        {
            if (!m_axis)
                return;
            if (event.Button == MouseButton::Left)
            {
                event.Handled = true;
                const f32 x = LocalX(event.Position);
                if (const i32 marker = MarkerAt(x); marker >= 0)
                {
                    // A marker pressed is chosen; dragged past the threshold, it moves.
                    ChooseMarker(marker);
                    m_markerDrag = MarkerDrag::Pending;
                    m_pressX = x;
                    return;
                }
                m_scrubbing = true;
                ScrubTo(event.Position);
            }
            else if (event.Button == MouseButton::Middle)
            {
                event.Handled = true;
                m_panning = true;
                m_panFrom = event.Position.x;
                m_panScroll = m_axis->Scroll();
            }
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_axis)
                return;
            const f32 x = LocalX(event.Position);
            if (m_markerDrag == MarkerDrag::Pending && m_markers && m_chosen >= 0 && core::Abs(x - m_pressX) >= kDragThreshold)
            {
                m_markerDrag = MarkerDrag::Moving;
                m_markers->BeginMarkerMove(static_cast<usize>(m_chosen));
            }
            if (m_markerDrag == MarkerDrag::Moving)
            {
                m_markers->PreviewMarkerMove(static_cast<usize>(m_chosen), core::Clamp(m_axis->Snap(m_axis->OffsetToTime(x)), 0.0f, m_axis->Duration()));
                Invalidate();
            }
            else if (m_markerDrag == MarkerDrag::None && !m_scrubbing && !m_panning)
                Hover(MarkerAt(x));
            if (m_scrubbing)
                ScrubTo(event.Position);
            else if (m_panning)
                m_axis->SetScroll(m_panScroll - (event.Position.x - m_panFrom) / m_axis->PixelsPerSecond());
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            if (m_markerDrag != MarkerDrag::None && event.Button == MouseButton::Left)
                EndMarkerDrag(true);
            if (m_scrubbing && event.Button == MouseButton::Left)
                m_scrubbing = false;
            if (m_panning && event.Button == MouseButton::Middle)
                m_panning = false;
        }
        void OnPointerCancel(const MouseEvent&) override
        {
            EndMarkerDrag(false);
            m_scrubbing = false;
            m_panning = false;
        }
        void OnMouseLeave(const MouseEvent& event) override
        {
            UIWidget::OnMouseLeave(event);
            if (m_markerDrag == MarkerDrag::None)
                Hover(-1);
        }
        void OnKeyDown(const KeyEvent& event) override
        {
            if (static_cast<KeyCode>(event.KeyCode) == KeyCode::Escape && m_markerDrag == MarkerDrag::Moving)
            {
                event.Handled = true;
                EndMarkerDrag(false);
            }
        }
        void OnMouseWheel(const WheelEvent& event) override
        {
            if (!m_axis || event.Delta.y == 0.0f)
                return;
            event.Handled = true;
            m_axis->ZoomAbout(TimeAt(event.Position), core::Pow(1.2f, event.Delta.y));
        }
        [[nodiscard]] Cursor GetCursorNow() const override { return m_panning ? Cursor::Grabbing : UIWidget::GetCursorNow(); }

        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetFontSelection(GetFontService(), GetTextStyle().FontFamily.AsView(), GetTextStyle().Selection);
            m_text.SetColor(GetTextColor());
            m_text.SetAlignment(TextHAlign::Start, TextVAlign::Top);
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            UIWidget::OnDraw(ctx, localBounds);
            if (!m_axis)
                return;
            const Rect box = GetContentBounds();
            const PartStyle& tickPart = GetPartStyle(u8"tick");
            Color tickColor = tickPart.HasBackground ? tickPart.Box.BackgroundColor : GetTextColor();
            tickColor.a *= tickPart.Opacity;
            core::Array<TimeTick> ticks;
            GetTicks(ticks);
            ctx.PushClip(box);
            for (const TimeTick& tick : ticks)
            {
                const f32 x = box.x + tick.Offset;
                const f32 length = tick.Major ? box.height * 0.5f : box.height * 0.25f;
                ctx.VG().FillRect(Rectangle{core::Floor(x), box.Bottom() - length, 1.0f, length}, tickColor);
                if (tick.Major && m_text.HasFont())
                {
                    m_text.SetString(tick.Label.AsView());
                    m_text.Draw(ctx, core::Float2{x + 3.0f, box.y + 1.0f});
                }
            }
            if (m_markers)
            {
                // Each a line down the ruler and a flag at its foot, the chosen one in its own colour.
                const PartStyle& markerPart = GetPartStyle(u8"marker");
                const PartStyle& chosenPart = GetPartStyle(u8"marker-chosen");
                Color markerColor = markerPart.HasBackground ? markerPart.Box.BackgroundColor : GetTextColor();
                markerColor.a *= markerPart.Opacity;
                Color chosenColor = chosenPart.HasBackground ? chosenPart.Box.BackgroundColor : Color{0.31f, 0.61f, 1.0f, 1.0f};
                chosenColor.a *= chosenPart.Opacity;
                for (usize i = 0; i < m_markers->MarkerCount(); ++i)
                {
                    const f32 mx = box.x + m_axis->TimeToOffset(m_markers->GetMarker(i).Time);
                    if (mx < box.x - 6.0f || mx > box.Right() + 6.0f)
                        continue;
                    const Color color = static_cast<i32>(i) == m_chosen ? chosenColor : markerColor;
                    ctx.VG().FillRect(Rectangle{core::Floor(mx), box.y, 1.0f, box.height}, color);
                    vg::PathBuilder flag;
                    flag.MoveTo(mx, box.Bottom() - 9.0f);
                    flag.LineTo(mx + 4.0f, box.Bottom() - 4.5f);
                    flag.LineTo(mx, box.Bottom());
                    flag.LineTo(mx - 4.0f, box.Bottom() - 4.5f);
                    flag.Close();
                    ctx.VG().FillPath(flag.ToPath(), color);
                }
            }
            const PartStyle& playheadPart = GetPartStyle(u8"playhead");
            Color playhead = playheadPart.HasBackground ? playheadPart.Box.BackgroundColor : Color{0.31f, 0.61f, 1.0f, 1.0f};
            playhead.a *= playheadPart.Opacity;
            const f32 x = box.x + m_axis->TimeToOffset(m_axis->Playhead());
            ctx.VG().FillRect(Rectangle{core::Floor(x) - 1.0f, box.y, 2.0f, box.height}, playhead);
            vg::PathBuilder head;
            head.MoveTo(x - 5.0f, box.y);
            head.LineTo(x + 5.0f, box.y);
            head.LineTo(x, box.y + 6.0f);
            head.Close();
            ctx.VG().FillPath(head.ToPath(), playhead);
            ctx.PopClip();
        }

    private:
        // A labelled step's minor ticks: fifths of a 1 or 5, halves of a 2; a frame's none.
        [[nodiscard]] f32 MinorDivisions(f32 step) const noexcept
        {
            const f32 units = m_axis->FrameRate() > 0.0f ? step * m_axis->FrameRate() : step;
            const f32 lead = units / core::Pow(10.0f, core::Floor((core::Log(units) / core::Log(10.0f)) + 0.0001f));
            const f32 divisions = core::Abs(lead - 2.0f) < 0.01f ? 2.0f : 5.0f;
            if (m_axis->FrameRate() > 0.0f && units / divisions < 1.0f)
                return core::Max(1.0f, core::Floor(units)); // never between frames
            return divisions;
        }
        [[nodiscard]] f32 TimeAt(core::Float2 screen) const
        {
            const core::Float2 local = ConvertToNodeSpace(screen); // events come in screen space
            return m_axis->OffsetToTime(local.x - GetContentBounds().x);
        }
        void ScrubTo(core::Float2 screen) { m_axis->SetPlayhead(m_axis->Snap(TimeAt(screen))); }
        [[nodiscard]] f32 LocalX(core::Float2 screen) const { return ConvertToNodeSpace(screen).x - GetContentBounds().x; }

        void Hover(i32 marker)
        {
            if (marker == m_hover)
                return;
            m_hover = marker;
            m_hoverTip.Clear();
            if (m_markers && marker >= 0)
            {
                const TimeMarker shown = m_markers->GetMarker(static_cast<usize>(marker));
                m_hoverTip = m_axis ? core::Format(u8"{} at {}", shown.Label, m_axis->FormatTime(shown.Time, 0.01f)) : core::String(shown.Label.AsView());
            }
        }
        void EndMarkerDrag(bool commit)
        {
            const bool moving = m_markerDrag == MarkerDrag::Moving;
            m_markerDrag = MarkerDrag::None;
            if (!moving || !m_markers)
                return;
            if (commit)
                m_markers->CommitMarkerMove();
            else
                m_markers->CancelMarkerMove();
            Invalidate();
        }

        enum class MarkerDrag : u8
        {
            None,
            Pending, // pressed, not yet past the threshold
            Moving,
        };

        core::RefPtr<TimeAxis> m_axis;
        ITimeMarkerSource* m_markers = nullptr;
        i32 m_chosen = -1;
        i32 m_hover = -1;
        core::String m_hoverTip;
        MarkerDrag m_markerDrag = MarkerDrag::None;
        f32 m_pressX = 0.0f;
        mutable Text m_text; // each label in turn
        bool m_scrubbing = false;
        bool m_panning = false;
        f32 m_panFrom = 0.0f;
        f32 m_panScroll = 0.0f;
    };
}
