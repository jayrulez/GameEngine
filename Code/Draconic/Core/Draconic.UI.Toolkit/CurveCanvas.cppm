// Draconic UI Toolkit - :curve_canvas partition
//
// Multi-channel interactive curve editor canvas. Model-agnostic: callers describe their channels via
// SetChannels, push initial keys via SetKeys(channelIdx, ...), and listen to OnKey* events to project
// mutations back to their domain types. Reusable for any keyframe-based authoring - particle curves,
// animation easing, audio envelopes, tone-mapping LUTs, post-FX over time, gameplay tuning curves.
// Ported from Sedulous.UI.Toolkit/src/Controls/CurveCanvas.bf (a View).
//
// Port taxes: Beef `List<T>` -> Array<T>; the Beef private inner `Channel` heap class (List<Channel*> +
// DeleteContainerAndItems) becomes a plain value struct holding Array<Key>, stored Array<Channel> BY VALUE
// (RAII owns everything). `Event<delegate void(...)> ~ _.Dispose()` -> Event<void(...)>; byte
// Color(r,g,b,a) -> private static Rgb(); `out` params -> reference out-params; `scope $"{v:0.##}"` ->
// FormatShort (snprintf %.2f + trailing-zero trim). Immediate-path VG calls port 1:1.

module;
#include <cstdio>
#include <limits>
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:curve_canvas;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Interpolation modes available per-channel on CurveCanvas. Hermite is first (=0) so a
    /// zero-initialized ChannelDescriptor defaults to Hermite, the right pick for particle / animation curves.
    enum class CurveInterpolation : u8
    {
        /// Cubic Hermite using per-key TangentIn / TangentOut.
        Hermite,
        /// Linear between adjacent keys; tangents ignored.
        Linear,
        /// Step / hold; value jumps to the next key's value at that key's Time.
        Step
    };

    /// Tangent-handle behavior on a Hermite key. Mirrored is the zero-init default.
    enum class TangentMode : u8
    {
        /// Drag one handle and the opposite handle mirrors it (TangentIn == TangentOut). The smooth default.
        Mirrored,
        /// Handles move independently. Use for sharp corners or asymmetric ease in/out ("Broken" in some DCC tools).
        Free,
        /// Both tangents pinned to zero - the curve flattens through the key. Handles are visible but ignore drag.
        Flat
    };

    /// Per-channel metadata pushed into CurveCanvas via SetChannels. Lets the widget render N curves on the
    /// same canvas without knowing what the channels represent. All fields default sensibly on zero-init.
    struct ChannelDescriptor
    {
        /// Short identifier shown in legend strips (e.g. "X", "R", "Gain").
        String Name;
        /// Stroke color used for the polyline and key markers.
        foundation::Color StrokeColor{0, 0, 0, 0};
        /// Value used when a key is added to this channel implicitly (LinkedTime fallback value).
        f32 DefaultValue = 0.0f;
        /// When true, the channel is hidden (not rendered or hit-tested). Inverted polarity: zero-init = visible.
        bool Hidden = false;
        /// When true, the channel rejects edits but still renders.
        bool Locked = false;
        /// Inclusive value clamps applied on edit. If MinValue >= MaxValue the clamp is disabled.
        f32 MinValue = 0.0f;
        f32 MaxValue = 0.0f;
        /// Nominal value-axis range. When DisplayMin < DisplayMax the canvas frames at least this range.
        f32 DisplayMin = 0.0f;
        f32 DisplayMax = 0.0f;
        /// How this channel's curve interpolates between keys.
        CurveInterpolation Interpolation = CurveInterpolation::Hermite;
        /// Optional longer description (reserved for hover tooltips; not yet rendered by the canvas).
        String Description;
    };

    /// Multi-channel interactive curve editor canvas.
    class CurveCanvas : public View
    {
        DRACONIC_OBJECT(CurveCanvas, View)
    public:
        /// One keypoint. Times in [0, 1]; Values in user space. Tangents are slope (dy/dt) at the key.
        struct Key
        {
            f32 Time = 0.0f;
            f32 Value = 0.0f;
            f32 TangentIn = 0.0f;
            f32 TangentOut = 0.0f;
            TangentMode Mode = TangentMode::Mirrored;

            Key() = default;
            Key(f32 time, f32 value, f32 tangentIn = 0.0f, f32 tangentOut = 0.0f,
                TangentMode mode = TangentMode::Mirrored)
                : Time(time), Value(value), TangentIn(tangentIn), TangentOut(tangentOut), Mode(mode)
            {
            }
        };

        /// Max keypoints per channel. Default matches the particle limit.
        i32 MaxKeys = 8;

        /// When true, all channels share a single Time axis: every channel has the same KeyCount and the
        /// i-th key has the same Time across all channels.
        bool LinkedTime = false;

        /// When true, ValueMin / ValueMax are recomputed from current visible keys (with a small margin).
        bool AutoFitValueRange = true;

        f32 ValueMin = 0.0f;
        f32 ValueMax = 1.0f;

        /// Fired when an edit gesture begins (mouse-down starting a drag, or click-add, or right-click delete).
        Event<void()> OnEditBegin;
        /// Fired when the current edit gesture commits.
        Event<void()> OnEditEnd;
        /// Fired when an existing key was repositioned. Payload: channel index, key index.
        Event<void(i32, i32)> OnKeyChanged;
        /// Fired after a new key was inserted.
        Event<void(i32, i32)> OnKeyAdded;
        /// Fired after an existing key was deleted; payload carries the OLD index (before removal).
        Event<void(i32, i32)> OnKeyRemoved;

        [[nodiscard]] i32 ChannelCount() const { return static_cast<i32>(m_channels.Size()); }
        [[nodiscard]] i32 SelectedChannel() const { return m_selectedChannelIdx; }
        [[nodiscard]] i32 SelectedKeyIndex() const { return m_selectedKeyIdx; }

        [[nodiscard]] ChannelDescriptor GetChannelDescriptor(i32 idx) const
        {
            return m_channels[static_cast<usize>(idx)].Descriptor;
        }
        [[nodiscard]] i32 GetKeyCount(i32 channelIdx) const
        {
            return static_cast<i32>(m_channels[static_cast<usize>(channelIdx)].Keys.Size());
        }
        [[nodiscard]] Key GetKey(i32 channelIdx, i32 keyIdx) const
        {
            return m_channels[static_cast<usize>(channelIdx)].Keys[static_cast<usize>(keyIdx)];
        }

        /// Configure the channel set. Clears existing keys. Selection resets to channel 0 if any channels exist.
        void SetChannels(Span<const ChannelDescriptor> channels)
        {
            ClearChannels();
            for (usize i = 0; i < channels.Size(); ++i)
            {
                Channel ch;
                ch.Descriptor = channels[i];
                m_channels.PushBack(ch);
            }
            m_selectedChannelIdx = m_channels.Size() > 0 ? 0 : -1;
            m_selectedKeyIdx = -1;
            m_draggingChannelIdx = -1;
            m_draggingKeyIdx = -1;
            Invalidate();
        }

        /// Replace one channel's keys. Does not enforce LinkedTime alignment.
        void SetKeys(i32 channelIdx, Span<const Key> keys)
        {
            if (channelIdx < 0 || channelIdx >= static_cast<i32>(m_channels.Size()))
            {
                return;
            }
            Channel& ch = m_channels[static_cast<usize>(channelIdx)];
            ch.Keys.Clear();
            for (usize i = 0; i < keys.Size(); ++i)
            {
                ch.Keys.PushBack(keys[i]);
            }
            if (m_draggingChannelIdx == channelIdx)
            {
                m_draggingChannelIdx = -1;
                m_draggingKeyIdx = -1;
            }
            if (m_selectedChannelIdx == channelIdx)
            {
                m_selectedKeyIdx = -1;
            }
            Invalidate();
        }

        // === Mouse ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (m_channels.Size() == 0)
            {
                return;
            }

            // Tangent handles take priority over key markers - they're only drawn for the selected key.
            bool handleOutgoing = false;
            if (e.Button == MouseButton::Left && HitHandle(e.X, e.Y, handleOutgoing))
            {
                const Channel& ch = m_channels[static_cast<usize>(m_selectedChannelIdx)];
                const Key& k = ch.Keys[static_cast<usize>(m_selectedKeyIdx)];
                // Flat keys ignore drag - user must right-click to switch mode first.
                if (k.Mode == TangentMode::Flat)
                {
                    e.Handled = true;
                    return;
                }
                m_draggingChannelIdx = m_selectedChannelIdx;
                m_draggingKeyIdx = m_selectedKeyIdx;
                m_draggingHandle = handleOutgoing ? DraggingHandle::Out : DraggingHandle::In;
                BeginGesture();
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
                Invalidate();
                return;
            }

            if (e.Button == MouseButton::Right && HitHandle(e.X, e.Y, handleOutgoing))
            {
                const Channel& ch = m_channels[static_cast<usize>(m_selectedChannelIdx)];
                if (ch.Descriptor.Locked)
                {
                    return;
                }
                BeginGesture();
                CycleSelectedTangentMode();
                OnKeyChanged.Invoke(m_selectedChannelIdx, m_selectedKeyIdx);
                EndGesture();
                e.Handled = true;
                Invalidate();
                return;
            }

            if (e.Button == MouseButton::Left)
            {
                i32 hitCh = -1;
                i32 hitKey = -1;
                if (HitKey(e.X, e.Y, hitCh, hitKey))
                {
                    if (m_channels[static_cast<usize>(hitCh)].Descriptor.Locked)
                    {
                        return;
                    }
                    m_selectedChannelIdx = hitCh;
                    m_selectedKeyIdx = hitKey;
                    m_draggingChannelIdx = hitCh;
                    m_draggingKeyIdx = hitKey;
                    BeginGesture();
                    if (Context != nullptr)
                    {
                        Context->GetFocusManager()->SetCapture(this);
                    }
                    e.Handled = true;
                    Invalidate();
                    return;
                }

                // Empty click - add a key.
                const i32 activeIdx = ResolveActiveChannel();
                if (activeIdx < 0)
                {
                    return;
                }
                const Channel& activeCh = m_channels[static_cast<usize>(activeIdx)];
                if (static_cast<i32>(activeCh.Keys.Size()) >= MaxKeys)
                {
                    return;
                }

                const f32 t = XToTime(e.X);
                const f32 v = ClampToChannel(activeIdx, YToValue(e.Y));

                BeginGesture();
                if (LinkedTime)
                {
                    // Update every channel BEFORE firing any events so listeners see consistent state.
                    const i32 channelN = static_cast<i32>(m_channels.Size());
                    Array<i32> addedIndices;
                    addedIndices.Resize(static_cast<usize>(channelN));
                    i32 newIdx = -1;
                    for (i32 c = 0; c < channelN; c++)
                    {
                        const f32 chVal =
                            (c == activeIdx)
                                ? v
                                : ClampToChannel(
                                      c, m_channels[static_cast<usize>(c)].Descriptor.DefaultValue);
                        addedIndices[static_cast<usize>(c)] = InsertSortedKey(c, Key(t, chVal));
                        if (c == activeIdx)
                        {
                            newIdx = addedIndices[static_cast<usize>(c)];
                        }
                    }
                    m_selectedChannelIdx = activeIdx;
                    m_selectedKeyIdx = newIdx;
                    m_draggingChannelIdx = activeIdx;
                    m_draggingKeyIdx = newIdx;
                    for (i32 c = 0; c < channelN; c++)
                    {
                        OnKeyAdded.Invoke(c, addedIndices[static_cast<usize>(c)]);
                    }
                }
                else
                {
                    const i32 idx = InsertSortedKey(activeIdx, Key(t, v));
                    m_selectedChannelIdx = activeIdx;
                    m_selectedKeyIdx = idx;
                    m_draggingChannelIdx = activeIdx;
                    m_draggingKeyIdx = idx;
                    OnKeyAdded.Invoke(activeIdx, idx);
                }
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->SetCapture(this);
                }
                e.Handled = true;
                Invalidate();
            }
            else if (e.Button == MouseButton::Right)
            {
                i32 hitCh = -1;
                i32 hitKey = -1;
                if (HitKey(e.X, e.Y, hitCh, hitKey))
                {
                    if (m_channels[static_cast<usize>(hitCh)].Descriptor.Locked)
                    {
                        return;
                    }
                    BeginGesture();
                    if (LinkedTime)
                    {
                        // Remove from every channel first, then fire events.
                        Array<i32> removedChannels;
                        const i32 channelN = static_cast<i32>(m_channels.Size());
                        for (i32 c = 0; c < channelN; c++)
                        {
                            if (hitKey <
                                static_cast<i32>(m_channels[static_cast<usize>(c)].Keys.Size()))
                            {
                                m_channels[static_cast<usize>(c)].Keys.RemoveAt(
                                    static_cast<usize>(hitKey));
                                removedChannels.PushBack(c);
                            }
                        }
                        for (usize i = 0; i < removedChannels.Size(); ++i)
                        {
                            OnKeyRemoved.Invoke(removedChannels[i], hitKey);
                        }
                    }
                    else
                    {
                        m_channels[static_cast<usize>(hitCh)].Keys.RemoveAt(
                            static_cast<usize>(hitKey));
                        OnKeyRemoved.Invoke(hitCh, hitKey);
                    }
                    if (m_selectedChannelIdx == hitCh && m_selectedKeyIdx == hitKey)
                    {
                        m_selectedKeyIdx = -1;
                    }
                    EndGesture();
                    e.Handled = true;
                    Invalidate();
                }
            }
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            if (m_draggingChannelIdx < 0 || m_draggingKeyIdx < 0)
            {
                return;
            }
            const i32 dragCh = m_draggingChannelIdx;
            if (dragCh >= static_cast<i32>(m_channels.Size()))
            {
                return;
            }
            if (m_draggingKeyIdx >=
                static_cast<i32>(m_channels[static_cast<usize>(dragCh)].Keys.Size()))
            {
                return;
            }

            // Tangent-handle drag: keep the key in place and rotate the handle around it.
            if (m_draggingHandle != DraggingHandle::None)
            {
                Channel& ch = m_channels[static_cast<usize>(dragCh)];
                Key k = ch.Keys[static_cast<usize>(m_draggingKeyIdx)];
                const f32 kx = TimeToX(k.Time);
                const f32 ky = ValueToY(k.Value);
                f32 dx = e.X - kx;
                f32 dy = e.Y - ky;
                // Constrain dx to the handle's side so the user can't flip the In handle to the right.
                const bool outgoing = (m_draggingHandle == DraggingHandle::Out);
                const f32 minDx = 4.0f;
                if (outgoing && dx < minDx)
                {
                    dx = minDx;
                }
                if (!outgoing && dx > -minDx)
                {
                    dx = -minDx;
                }
                const f32 newSlope = HandleScreenToDataSlope(dx, dy);
                if (outgoing)
                {
                    k.TangentOut = newSlope;
                    if (k.Mode == TangentMode::Mirrored)
                    {
                        k.TangentIn = newSlope;
                    }
                }
                else
                {
                    k.TangentIn = newSlope;
                    if (k.Mode == TangentMode::Mirrored)
                    {
                        k.TangentOut = newSlope;
                    }
                }
                ch.Keys[static_cast<usize>(m_draggingKeyIdx)] = k;
                OnKeyChanged.Invoke(dragCh, m_draggingKeyIdx);
                e.Handled = true;
                Invalidate();
                return;
            }

            const f32 newTime = XToTime(e.X);
            const f32 newValue = ClampToChannel(dragCh, YToValue(e.Y));

            if (LinkedTime)
            {
                // Update Time on all channels at this index; Value only on active.
                const i32 channelN = static_cast<i32>(m_channels.Size());
                for (i32 c = 0; c < channelN; c++)
                {
                    if (m_draggingKeyIdx >=
                        static_cast<i32>(m_channels[static_cast<usize>(c)].Keys.Size()))
                    {
                        continue;
                    }
                    Key k = m_channels[static_cast<usize>(c)]
                                .Keys[static_cast<usize>(m_draggingKeyIdx)];
                    k.Time = newTime;
                    if (c == dragCh)
                    {
                        k.Value = newValue;
                    }
                    m_channels[static_cast<usize>(c)].Keys[static_cast<usize>(m_draggingKeyIdx)] =
                        k;
                }
                ReSortLinked(dragCh);
            }
            else
            {
                Channel& ch = m_channels[static_cast<usize>(dragCh)];
                Key k = ch.Keys[static_cast<usize>(m_draggingKeyIdx)];
                k.Time = newTime;
                k.Value = newValue;
                ch.Keys[static_cast<usize>(m_draggingKeyIdx)] = k;
                const Key moved = ch.Keys[static_cast<usize>(m_draggingKeyIdx)];
                ch.Keys.RemoveAt(static_cast<usize>(m_draggingKeyIdx));
                const i32 newIdx = InsertSortedKey(dragCh, moved);
                m_draggingKeyIdx = newIdx;
                m_selectedKeyIdx = newIdx;
            }

            OnKeyChanged.Invoke(dragCh, m_draggingKeyIdx);
            if (LinkedTime)
            {
                const i32 channelN = static_cast<i32>(m_channels.Size());
                for (i32 c = 0; c < channelN; c++)
                {
                    if (c != dragCh &&
                        m_draggingKeyIdx <
                            static_cast<i32>(m_channels[static_cast<usize>(c)].Keys.Size()))
                    {
                        OnKeyChanged.Invoke(c, m_draggingKeyIdx);
                    }
                }
            }

            e.Handled = true;
            Invalidate();
        }

        void OnMouseUp(MouseEventArgs& e) override
        {
            if (m_draggingChannelIdx >= 0)
            {
                m_draggingChannelIdx = -1;
                m_draggingKeyIdx = -1;
                m_draggingHandle = DraggingHandle::None;
                if (Context != nullptr)
                {
                    Context->GetFocusManager()->ReleaseCapture();
                }
                EndGesture();
                e.Handled = true;
            }
        }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            UpdateAutoFit();

            // Background.
            ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Rgb(28, 28, 33, 255));

            // Grid: 4 divisions on each axis (5 lines).
            constexpr i32 DIVS = 4;
            const foundation::Color gridColor = Rgb(50, 50, 58, 255);
            const foundation::Color labelColor = Rgb(120, 122, 132, 255);
            fonts::CachedFont* font =
                (ctx.FontService() != nullptr) ? ctx.FontService()->GetFont(9.0f) : nullptr;

            for (i32 i = 0; i <= DIVS; i++)
            {
                const f32 x = (i / static_cast<f32>(DIVS)) * Width();
                ctx.VG().FillRect(Rectangle{x, 0, 1, Height()}, gridColor);
            }
            for (i32 i = 0; i <= DIVS; i++)
            {
                const f32 y = (i / static_cast<f32>(DIVS)) * Height();
                ctx.VG().FillRect(Rectangle{0, foundation::Min(y, Height() - 1), Width(), 1}, gridColor);
            }

            if (font != nullptr)
            {
                // Y-axis value labels (top = ValueMax, bottom = ValueMin).
                for (i32 i = 0; i <= DIVS; i++)
                {
                    const f32 frac = i / static_cast<f32>(DIVS);
                    const f32 val = ValueMax - frac * (ValueMax - ValueMin);
                    const f32 lineY = frac * Height();
                    const String txt = FormatShort(val);
                    if (i == 0)
                    {
                        ctx.VG().DrawText(txt, font, Rectangle{2, 1, 42, 14},
                                          fonts::TextAlignment::Left, fonts::VerticalAlignment::Top,
                                          labelColor);
                    }
                    else if (i == DIVS)
                    {
                        ctx.VG().DrawText(txt, font, Rectangle{2, Height() - 15, 42, 14},
                                          fonts::TextAlignment::Left,
                                          fonts::VerticalAlignment::Bottom, labelColor);
                    }
                    else
                    {
                        ctx.VG().DrawText(txt, font, Rectangle{2, lineY - 7, 42, 14},
                                          fonts::TextAlignment::Left,
                                          fonts::VerticalAlignment::Middle, labelColor);
                    }
                }

                // X-axis time labels along the bottom.
                for (i32 i = 0; i <= DIVS; i++)
                {
                    const f32 frac = i / static_cast<f32>(DIVS);
                    const f32 lineX = frac * Width();
                    const String txt = FormatShort(frac);
                    if (i == 0)
                    {
                        ctx.VG().DrawText(txt, font, Rectangle{2, Height() - 13, 32, 12},
                                          fonts::TextAlignment::Left,
                                          fonts::VerticalAlignment::Bottom, labelColor);
                    }
                    else if (i == DIVS)
                    {
                        ctx.VG().DrawText(txt, font, Rectangle{Width() - 34, Height() - 13, 32, 12},
                                          fonts::TextAlignment::Right,
                                          fonts::VerticalAlignment::Bottom, labelColor);
                    }
                    else
                    {
                        ctx.VG().DrawText(txt, font, Rectangle{lineX - 16, Height() - 13, 32, 12},
                                          fonts::TextAlignment::Center,
                                          fonts::VerticalAlignment::Bottom, labelColor);
                    }
                }
            }

            // Draw each visible channel - polyline first, then markers.
            const i32 channelN = static_cast<i32>(m_channels.Size());
            for (i32 c = 0; c < channelN; c++)
            {
                const Channel& ch = m_channels[static_cast<usize>(c)];
                if (ch.Descriptor.Hidden || ch.Keys.Size() == 0)
                {
                    continue;
                }

                constexpr i32 SAMPLES = 128;
                ctx.VG().BeginPath();
                for (i32 i = 0; i <= SAMPLES; i++)
                {
                    const f32 t = i / static_cast<f32>(SAMPLES);
                    const f32 v = Evaluate(c, t);
                    const f32 x = TimeToX(t);
                    const f32 y = ValueToY(v);
                    if (i == 0)
                    {
                        ctx.VG().MoveTo(x, y);
                    }
                    else
                    {
                        ctx.VG().LineTo(x, y);
                    }
                }
                ctx.VG().Stroke(ch.Descriptor.StrokeColor, 1.5f);

                for (i32 i = 0; i < static_cast<i32>(ch.Keys.Size()); i++)
                {
                    const f32 cx = TimeToX(ch.Keys[static_cast<usize>(i)].Time);
                    const f32 cy = ValueToY(ch.Keys[static_cast<usize>(i)].Value);
                    const bool isSel = (c == m_selectedChannelIdx && i == m_selectedKeyIdx);
                    ctx.VG().FillCircle(Float2{cx, cy}, KeyDrawRadius,
                                        isSel ? Rgb(255, 220, 100, 255)
                                              : ch.Descriptor.StrokeColor);
                    if (isSel)
                    {
                        ctx.VG().BeginPath();
                        for (i32 a = 0; a <= 32; a++)
                        {
                            const f32 theta = (a / 32.0f) * foundation::kTwoPi;
                            const f32 px = cx + (KeyDrawRadius + 2) * Cos(theta);
                            const f32 py = cy + (KeyDrawRadius + 2) * Sin(theta);
                            if (a == 0)
                            {
                                ctx.VG().MoveTo(px, py);
                            }
                            else
                            {
                                ctx.VG().LineTo(px, py);
                            }
                        }
                        ctx.VG().Stroke(Rgb(255, 220, 100, 255), 1.5f);
                    }
                }

                // Tangent handles - drawn only on the selected key of a Hermite channel.
                if (c == m_selectedChannelIdx && m_selectedKeyIdx >= 0 &&
                    m_selectedKeyIdx < static_cast<i32>(ch.Keys.Size()) &&
                    ch.Descriptor.Interpolation == CurveInterpolation::Hermite)
                {
                    const Key& k = ch.Keys[static_cast<usize>(m_selectedKeyIdx)];
                    const f32 kx = TimeToX(k.Time);
                    const f32 ky = ValueToY(k.Value);

                    foundation::Color colIn{0, 0, 0, 0};
                    foundation::Color colOut{0, 0, 0, 0};
                    switch (k.Mode)
                    {
                    case TangentMode::Mirrored:
                        colIn = Rgb(180, 200, 255, 255);
                        colOut = Rgb(180, 200, 255, 255);
                        break;
                    case TangentMode::Free:
                        colIn = Rgb(255, 140, 120, 255);
                        colOut = Rgb(120, 220, 160, 255);
                        break;
                    case TangentMode::Flat:
                        colIn = Rgb(120, 122, 132, 255);
                        colOut = Rgb(120, 122, 132, 255);
                        break;
                    }

                    f32 ohx = 0.0f;
                    f32 ohy = 0.0f;
                    f32 ihx = 0.0f;
                    f32 ihy = 0.0f;
                    ComputeHandlePos(c, m_selectedKeyIdx, true, ohx, ohy);
                    ComputeHandlePos(c, m_selectedKeyIdx, false, ihx, ihy);

                    ctx.VG().DrawLine(Float2{kx, ky}, Float2{ihx, ihy}, colIn, 1);
                    ctx.VG().DrawLine(Float2{kx, ky}, Float2{ohx, ohy}, colOut, 1);
                    ctx.VG().FillRect(Rectangle{ihx - HandleDrawRadius, ihy - HandleDrawRadius,
                                                HandleDrawRadius * 2, HandleDrawRadius * 2},
                                      colIn);
                    ctx.VG().FillRect(Rectangle{ohx - HandleDrawRadius, ohy - HandleDrawRadius,
                                                HandleDrawRadius * 2, HandleDrawRadius * 2},
                                      colOut);
                }
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(200.0f), constraints.ConstrainHeight(120.0f)};
        }

    private:
        enum class DraggingHandle : u8
        {
            None,
            In,
            Out
        };

        struct Channel
        {
            ChannelDescriptor Descriptor;
            Array<Key> Keys;
        };

        [[nodiscard]] static foundation::Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return foundation::Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        // Format a float as up to 2 decimals with trailing zeros (and a trailing dot) trimmed - the Beef
        // `{v:0.##}` interpolation.
        [[nodiscard]] static String FormatShort(f32 v)
        {
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
            if (n < 0)
            {
                return String();
            }
            i32 len = static_cast<i32>(n);
            bool hasDot = false;
            for (i32 i = 0; i < len; ++i)
            {
                if (buf[i] == '.')
                {
                    hasDot = true;
                    break;
                }
            }
            if (hasDot)
            {
                while (len > 0 && buf[len - 1] == '0')
                {
                    --len;
                }
                if (len > 0 && buf[len - 1] == '.')
                {
                    --len;
                }
            }
            return String(
                StringView(reinterpret_cast<const char8_t*>(buf), static_cast<usize>(len)));
        }

        void ClearChannels() { m_channels.Clear(); }

        // === Curve evaluation ===

        [[nodiscard]] f32 Evaluate(i32 channelIdx, f32 t) const
        {
            const Channel& ch = m_channels[static_cast<usize>(channelIdx)];
            const usize n = ch.Keys.Size();
            if (n == 0)
            {
                return 0.0f;
            }
            if (n == 1)
            {
                return ch.Keys[0].Value;
            }
            if (t <= ch.Keys[0].Time)
            {
                return ch.Keys[0].Value;
            }
            if (t >= ch.Keys[n - 1].Time)
            {
                return ch.Keys[n - 1].Value;
            }

            for (usize i = 0; i < n - 1; ++i)
            {
                const Key& a = ch.Keys[i];
                const Key& b = ch.Keys[i + 1];
                if (t >= a.Time && t <= b.Time)
                {
                    const f32 seg = b.Time - a.Time;
                    if (seg < 0.0001f)
                    {
                        return a.Value;
                    }
                    const f32 lt = (t - a.Time) / seg;
                    switch (ch.Descriptor.Interpolation)
                    {
                    case CurveInterpolation::Linear:
                        return a.Value + (b.Value - a.Value) * lt;
                    case CurveInterpolation::Step:
                        return a.Value;
                    case CurveInterpolation::Hermite:
                    {
                        const f32 lt2 = lt * lt;
                        const f32 lt3 = lt2 * lt;
                        return (2 * lt3 - 3 * lt2 + 1) * a.Value +
                               (lt3 - 2 * lt2 + lt) * (a.TangentOut * seg) +
                               (-2 * lt3 + 3 * lt2) * b.Value + (lt3 - lt2) * (b.TangentIn * seg);
                    }
                    }
                }
            }
            return ch.Keys[n - 1].Value;
        }

        // === Auto-fit value range ===

        void UpdateAutoFit()
        {
            if (!AutoFitValueRange)
            {
                return;
            }
            // Freeze the value range for the duration of an edit gesture (avoids a mid-drag feedback loop).
            if (m_inGesture)
            {
                return;
            }

            f32 lo = std::numeric_limits<f32>::max();
            f32 hi = std::numeric_limits<f32>::lowest();
            bool any = false;
            // Union of declared nominal display ranges across visible channels.
            f32 nomLo = std::numeric_limits<f32>::max();
            f32 nomHi = std::numeric_limits<f32>::lowest();
            bool hasNominal = false;
            for (usize c = 0; c < m_channels.Size(); ++c)
            {
                const Channel& ch = m_channels[c];
                if (ch.Descriptor.Hidden)
                {
                    continue;
                }
                const ChannelDescriptor& d = ch.Descriptor;
                if (d.DisplayMin < d.DisplayMax)
                {
                    if (d.DisplayMin < nomLo)
                    {
                        nomLo = d.DisplayMin;
                    }
                    if (d.DisplayMax > nomHi)
                    {
                        nomHi = d.DisplayMax;
                    }
                    hasNominal = true;
                }
                for (usize i = 0; i < ch.Keys.Size(); ++i)
                {
                    const f32 kv = ch.Keys[i].Value;
                    if (kv < lo)
                    {
                        lo = kv;
                    }
                    if (kv > hi)
                    {
                        hi = kv;
                    }
                    any = true;
                }
            }

            // Channel-declared nominal range: frame to it and only expand to admit outside keys.
            if (hasNominal)
            {
                f32 fLo = nomLo;
                f32 fHi = nomHi;
                if (any)
                {
                    const f32 pad = (nomHi - nomLo) * 0.05f;
                    if (lo < fLo)
                    {
                        fLo = lo - pad;
                    }
                    if (hi > fHi)
                    {
                        fHi = hi + pad;
                    }
                }
                ValueMin = fLo;
                ValueMax = fHi;
                return;
            }

            if (!any)
            {
                ValueMin = 0.0f;
                ValueMax = 1.0f;
                return;
            }

            const f32 center = (lo + hi) * 0.5f;
            const f32 span = hi - lo;

            // No nominal range declared: auto-fit with a span floor (avoids collapse when keys cluster).
            const f32 minSpan = foundation::Max(0.5f, Abs(center) * 0.5f);
            if (span < minSpan)
            {
                ValueMin = center - minSpan * 0.5f;
                ValueMax = center + minSpan * 0.5f;
            }
            else
            {
                const f32 margin = span * 0.1f;
                ValueMin = lo - margin;
                ValueMax = hi + margin;
            }
        }

        // === Coordinate transforms ===

        [[nodiscard]] f32 TimeToX(f32 t) const { return t * Width(); }
        [[nodiscard]] f32 ValueToY(f32 v) const
        {
            const f32 denom = (ValueMax - ValueMin);
            if (denom < 0.0001f)
            {
                return Height() * 0.5f;
            }
            return Height() * (1.0f - (v - ValueMin) / denom);
        }
        [[nodiscard]] f32 XToTime(f32 x) const { return foundation::Clamp(x / Width(), 0.0f, 1.0f); }
        [[nodiscard]] f32 YToValue(f32 y) const
        {
            const f32 r = foundation::Clamp(y / Height(), 0.0f, 1.0f);
            return ValueMax - r * (ValueMax - ValueMin);
        }

        // === Hit testing ===

        static constexpr f32 KeyHitRadius = 8.0f;
        static constexpr f32 KeyDrawRadius = 4.0f;
        static constexpr f32 HandleScreenLen = 32.0f;
        static constexpr f32 HandleHitRadius = 7.0f;
        static constexpr f32 HandleDrawRadius = 4.0f;

        // Computes the screen position of one tangent handle.
        void ComputeHandlePos(i32 channelIdx, i32 keyIdx, bool outgoing, f32& hx, f32& hy) const
        {
            const Key& k =
                m_channels[static_cast<usize>(channelIdx)].Keys[static_cast<usize>(keyIdx)];
            const f32 kx = TimeToX(k.Time);
            const f32 ky = ValueToY(k.Value);
            const f32 pV =
                (ValueMax > ValueMin + 0.0001f) ? Height() / (ValueMax - ValueMin) : 0.0f;
            const f32 slope = outgoing ? k.TangentOut : k.TangentIn;
            const f32 dx = outgoing ? Width() : -Width();
            const f32 dy = outgoing ? (-slope * pV) : (slope * pV);
            const f32 norm = Sqrt(dx * dx + dy * dy);
            if (norm < 0.0001f)
            {
                hx = kx + (outgoing ? HandleScreenLen : -HandleScreenLen);
                hy = ky;
                return;
            }
            hx = kx + HandleScreenLen * dx / norm;
            hy = ky + HandleScreenLen * dy / norm;
        }

        // Inverse of ComputeHandlePos's slope projection.
        [[nodiscard]] f32 HandleScreenToDataSlope(f32 screenDx, f32 screenDy) const
        {
            if (Abs(screenDx) < 0.0001f)
            {
                return 0.0f;
            }
            const f32 pV =
                (ValueMax > ValueMin + 0.0001f) ? Height() / (ValueMax - ValueMin) : 1.0f;
            if (pV < 0.0001f)
            {
                return 0.0f;
            }
            const f32 screenSlope = screenDy / screenDx;
            return -screenSlope * Width() / pV;
        }

        // True iff (x, y) is within HandleHitRadius of either tangent handle on the selected key.
        [[nodiscard]] bool HitHandle(f32 x, f32 y, bool& outgoing) const
        {
            outgoing = false;
            if (m_selectedChannelIdx < 0 || m_selectedKeyIdx < 0)
            {
                return false;
            }
            if (m_selectedChannelIdx >= static_cast<i32>(m_channels.Size()))
            {
                return false;
            }
            const Channel& ch = m_channels[static_cast<usize>(m_selectedChannelIdx)];
            if (ch.Descriptor.Hidden || ch.Descriptor.Locked)
            {
                return false;
            }
            if (ch.Descriptor.Interpolation != CurveInterpolation::Hermite)
            {
                return false;
            }
            if (m_selectedKeyIdx >= static_cast<i32>(ch.Keys.Size()))
            {
                return false;
            }

            f32 hx = 0.0f;
            f32 hy = 0.0f;
            ComputeHandlePos(m_selectedChannelIdx, m_selectedKeyIdx, true, hx, hy);
            f32 dx = x - hx;
            f32 dy = y - hy;
            if (dx * dx + dy * dy <= HandleHitRadius * HandleHitRadius)
            {
                outgoing = true;
                return true;
            }
            ComputeHandlePos(m_selectedChannelIdx, m_selectedKeyIdx, false, hx, hy);
            dx = x - hx;
            dy = y - hy;
            if (dx * dx + dy * dy <= HandleHitRadius * HandleHitRadius)
            {
                outgoing = false;
                return true;
            }
            return false;
        }

        // Cycles the selected key's TangentMode: Mirrored -> Free -> Flat. Caller fires OnKeyChanged.
        void CycleSelectedTangentMode()
        {
            if (m_selectedChannelIdx < 0 || m_selectedKeyIdx < 0)
            {
                return;
            }
            Channel& ch = m_channels[static_cast<usize>(m_selectedChannelIdx)];
            if (m_selectedKeyIdx >= static_cast<i32>(ch.Keys.Size()))
            {
                return;
            }
            Key k = ch.Keys[static_cast<usize>(m_selectedKeyIdx)];
            switch (k.Mode)
            {
            case TangentMode::Mirrored:
                k.Mode = TangentMode::Free;
                break;
            case TangentMode::Free:
                k.Mode = TangentMode::Flat;
                k.TangentIn = 0.0f;
                k.TangentOut = 0.0f;
                break;
            case TangentMode::Flat:
                k.Mode = TangentMode::Mirrored;
                // Sync In to Out so the handles snap together visibly.
                k.TangentIn = k.TangentOut;
                break;
            }
            ch.Keys[static_cast<usize>(m_selectedKeyIdx)] = k;
        }

        bool HitKey(f32 x, f32 y, i32& channelIdx, i32& keyIdx)
        {
            channelIdx = -1;
            keyIdx = -1;
            UpdateAutoFit();

            // Prefer the currently selected channel so overlapping keys disambiguate in its favor.
            if (m_selectedChannelIdx >= 0 &&
                m_selectedChannelIdx < static_cast<i32>(m_channels.Size()))
            {
                if (HitKeyInChannel(m_selectedChannelIdx, x, y, keyIdx))
                {
                    channelIdx = m_selectedChannelIdx;
                    return true;
                }
            }
            const i32 channelN = static_cast<i32>(m_channels.Size());
            for (i32 c = 0; c < channelN; c++)
            {
                if (c == m_selectedChannelIdx)
                {
                    continue;
                }
                if (HitKeyInChannel(c, x, y, keyIdx))
                {
                    channelIdx = c;
                    return true;
                }
            }
            return false;
        }

        bool HitKeyInChannel(i32 channelIdx, f32 x, f32 y, i32& keyIdx) const
        {
            keyIdx = -1;
            const Channel& ch = m_channels[static_cast<usize>(channelIdx)];
            if (ch.Descriptor.Hidden)
            {
                return false;
            }
            for (i32 i = 0; i < static_cast<i32>(ch.Keys.Size()); i++)
            {
                const f32 kx = TimeToX(ch.Keys[static_cast<usize>(i)].Time);
                const f32 ky = ValueToY(ch.Keys[static_cast<usize>(i)].Value);
                const f32 dx = x - kx;
                const f32 dy = y - ky;
                if (dx * dx + dy * dy <= KeyHitRadius * KeyHitRadius)
                {
                    keyIdx = i;
                    return true;
                }
            }
            return false;
        }

        // === Gesture ===

        void BeginGesture()
        {
            if (!m_inGesture)
            {
                m_inGesture = true;
                OnEditBegin.Invoke();
            }
        }
        void EndGesture()
        {
            if (m_inGesture)
            {
                m_inGesture = false;
                OnEditEnd.Invoke();
            }
        }

        // === Helpers ===

        i32 ResolveActiveChannel() const
        {
            if (m_selectedChannelIdx >= 0 &&
                m_selectedChannelIdx < static_cast<i32>(m_channels.Size()))
            {
                const ChannelDescriptor& d =
                    m_channels[static_cast<usize>(m_selectedChannelIdx)].Descriptor;
                if (!d.Hidden && !d.Locked)
                {
                    return m_selectedChannelIdx;
                }
            }
            const i32 channelN = static_cast<i32>(m_channels.Size());
            for (i32 i = 0; i < channelN; i++)
            {
                const ChannelDescriptor& d = m_channels[static_cast<usize>(i)].Descriptor;
                if (!d.Hidden && !d.Locked)
                {
                    return i;
                }
            }
            return -1;
        }

        i32 InsertSortedKey(i32 channelIdx, Key k)
        {
            Channel& ch = m_channels[static_cast<usize>(channelIdx)];
            i32 idx = static_cast<i32>(ch.Keys.Size());
            for (i32 i = 0; i < static_cast<i32>(ch.Keys.Size()); i++)
            {
                if (ch.Keys[static_cast<usize>(i)].Time > k.Time)
                {
                    idx = i;
                    break;
                }
            }
            ch.Keys.Insert(static_cast<usize>(idx), k);
            return idx;
        }

        f32 ClampToChannel(i32 channelIdx, f32 v) const
        {
            const ChannelDescriptor& d = m_channels[static_cast<usize>(channelIdx)].Descriptor;
            // MinValue >= MaxValue (including the zero-init 0,0 case) means "no clamp configured".
            if (d.MinValue >= d.MaxValue)
            {
                return v;
            }
            return foundation::Clamp(v, d.MinValue, d.MaxValue);
        }

        // In LinkedTime mode after a drag, sort the driver in place and apply the same permutation
        // to every other channel so they stay aligned.
        void ReSortLinked(i32 driverIdx)
        {
            Channel& driverCh = m_channels[static_cast<usize>(driverIdx)];
            const i32 n = static_cast<i32>(driverCh.Keys.Size());
            if (n <= 1)
            {
                return;
            }

            // Build a permutation: indices[i] = old position of the i-th sorted key.
            Array<i32> indices;
            indices.Resize(static_cast<usize>(n));
            for (i32 i = 0; i < n; i++)
            {
                indices[static_cast<usize>(i)] = i;
            }
            for (i32 i = 1; i < n; i++)
            {
                const i32 cur = indices[static_cast<usize>(i)];
                const f32 curTime = driverCh.Keys[static_cast<usize>(cur)].Time;
                i32 j = i - 1;
                while (j >= 0 &&
                       driverCh.Keys[static_cast<usize>(indices[static_cast<usize>(j)])].Time >
                           curTime)
                {
                    indices[static_cast<usize>(j + 1)] = indices[static_cast<usize>(j)];
                    j--;
                }
                indices[static_cast<usize>(j + 1)] = cur;
            }

            // Apply the permutation to every channel.
            for (usize c = 0; c < m_channels.Size(); ++c)
            {
                Channel& ch = m_channels[c];
                if (static_cast<i32>(ch.Keys.Size()) != n)
                {
                    continue;
                } // Defensive: only re-sort matched channels.
                Array<Key> oldKeys;
                oldKeys.Resize(static_cast<usize>(n));
                for (i32 i = 0; i < n; i++)
                {
                    oldKeys[static_cast<usize>(i)] = ch.Keys[static_cast<usize>(i)];
                }
                for (i32 i = 0; i < n; i++)
                {
                    ch.Keys[static_cast<usize>(i)] =
                        oldKeys[static_cast<usize>(indices[static_cast<usize>(i)])];
                }
            }

            // Translate the dragging index through the permutation.
            for (i32 i = 0; i < n; i++)
            {
                if (indices[static_cast<usize>(i)] == m_draggingKeyIdx)
                {
                    m_draggingKeyIdx = i;
                    m_selectedKeyIdx = i;
                    break;
                }
            }
        }

        Array<Channel> m_channels;
        i32 m_selectedChannelIdx = -1;
        i32 m_selectedKeyIdx = -1;
        i32 m_draggingChannelIdx = -1;
        i32 m_draggingKeyIdx = -1;
        bool m_inGesture = false;
        DraggingHandle m_draggingHandle = DraggingHandle::None;
    };

    DRACONIC_DEFINE_OBJECT(CurveCanvas, "draconic::ui::toolkit")
}
