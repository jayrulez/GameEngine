// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :number_field partition
//
// NumberField: a text field for a number that can be typed, stepped or scrubbed, holding its
// value in a RangeValue.
//
// - The text shows the value with the step's number of decimals (none for an `integer` field),
//   between the `prefix` and `suffix`.
// - Typed text is read as an expression when it commits (on Return, or when focus leaves). It may
//   use numbers, +, -, *, / and parentheses. The result is snapped to the step and kept in the
//   range, unless the field allows values past it. Text that is not a number reverts to the
//   previous value.
// - While the field is not being edited, pressing and dragging across it scrubs the value once
//   the pointer has moved a few pixels (Shift for fine, Ctrl for coarse). Scrubbing sends `input`
//   as it goes and `change` on release. A press released without a drag starts editing with all
//   the text selected.
// - The arrows (repeat buttons built on ButtonBase) step the value, as do Up and Down, Page Up and
//   Page Down (by a page), and the wheel while the field is focused.
// - A value set from code notifies bindings only and sends no events, as with a range widget.
// - In the mixed state (several objects with different values, as in a property grid) no number
//   shows; the placeholder does. Whatever the user does next sets a value, even if it equals the
//   one held.
// - Escape during a scrub restores the value from before the press and reports the cancel.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:number_field;

import foundation.core; // String, Format, Optional, Float2, Max, Min, Abs, Function
import foundation.vg;   // PathBuilder
import :reactive;       // Observable
import :rect;
import :event;
import :draw_context;
import :node_box;
import :layout_style;
import :ui_widget;
import :button_base;
import :range_value;
import :text_field;
import :event_dispatcher; // CapturePointer (a scrub)

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    namespace detail
    {
        // This is at namespace scope rather than local to EvaluateExpression because clang does not
        // emit the members of a class local to an exported inline function for its importers.
        struct ExpressionParser
        {
            core::StringView s;
            usize i = 0;
            bool ok = true;
            void Skip()
            {
                while (i < s.Size() && (s[i] == u8' ' || s[i] == u8'\t'))
                    ++i;
            }
            f64 Primary()
            {
                Skip();
                if (i < s.Size() && (s[i] == u8'-' || s[i] == u8'+'))
                {
                    const bool negative = s[i++] == u8'-';
                    const f64 v = Primary();
                    return negative ? -v : v;
                }
                if (i < s.Size() && s[i] == u8'(')
                {
                    ++i;
                    const f64 v = Sum();
                    Skip();
                    if (i < s.Size() && s[i] == u8')')
                        ++i;
                    else
                        ok = false;
                    return v;
                }
                const usize start = i;
                while (i < s.Size() && ((s[i] >= u8'0' && s[i] <= u8'9') || s[i] == u8'.'))
                    ++i;
                const Optional<f64> v = i > start ? core::ParseFloat(s.SubStr(start, i - start)) : Optional<f64>{};
                if (!v.HasValue())
                {
                    ok = false;
                    return 0.0;
                }
                return v.Value();
            }
            f64 Product()
            {
                f64 v = Primary();
                for (;;)
                {
                    Skip();
                    if (i >= s.Size() || (s[i] != u8'*' && s[i] != u8'/'))
                        return v;
                    const bool divide = s[i++] == u8'/';
                    const f64 r = Primary();
                    if (divide && r == 0.0)
                        ok = false;
                    v = divide ? (r != 0.0 ? v / r : 0.0) : v * r;
                }
            }
            f64 Sum()
            {
                f64 v = Product();
                for (;;)
                {
                    Skip();
                    if (i >= s.Size() || (s[i] != u8'+' && s[i] != u8'-'))
                        return v;
                    const bool minus = s[i++] == u8'-';
                    const f64 r = Product();
                    v = minus ? v - r : v + r;
                }
            }
        };
    }

    /// Evaluates `text` as arithmetic: numbers (with a decimal point), +, -, *, /, unary minus and
    /// parentheses. Returns an empty Optional when it is not a valid expression or divides by zero.
    [[nodiscard]] inline Optional<f64> EvaluateExpression(core::StringView text)
    {
        detail::ExpressionParser p{text};
        const f64 v = p.Sum();
        p.Skip();
        if (!p.ok || p.i != text.Size() || text.Size() == 0)
            return {};
        return v;
    }

    /// A number field's arrow: a repeat button that acts on press. Its triangle takes the
    /// `spinbutton::arrow` part's background colour.
    class SpinButton : public ButtonBase
    {
        RTTI_OBJECT(SpinButton, ButtonBase)
    public:
        explicit SpinButton(bool up) : m_up(up)
        {
            SetTag(core::StringView(u8"spinbutton"));
            AddClass(core::StringView(up ? u8"up" : u8"down"));
            SetTabFocusable(false); // focus stays on the field
            SetRepeat(true);
            SetRepeatDelay(0.4f);
            SetRepeatInterval(0.06f);
        }
        [[nodiscard]] bool IsUp() const noexcept { return m_up; }

        void CollectStyleParts(core::Array<core::StringView>& out) const override { out.PushBack(core::StringView(u8"arrow")); }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetContentBounds();
            const PartStyle& arrow = GetPartStyle(u8"arrow");
            Color color = arrow.HasBackground ? arrow.Box.BackgroundColor : Color{0.70f, 0.74f, 0.80f, 1.0f};
            color.a *= arrow.Opacity;
            const f32 w = core::Min(8.0f, b.width * 0.6f), h = w * 0.55f;
            const f32 x = b.x + (b.width - w) * 0.5f, y = b.y + (b.height - h) * 0.5f;
            vg::PathBuilder pb;
            if (m_up)
            {
                pb.MoveTo(x, y + h);
                pb.LineTo(x + w, y + h);
                pb.LineTo(x + w * 0.5f, y);
            }
            else
            {
                pb.MoveTo(x, y);
                pb.LineTo(x + w, y);
                pb.LineTo(x + w * 0.5f, y + h);
            }
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), color);
        }

    private:
        bool m_up;
    };

    class NumberField : public TextField
    {
        RTTI_OBJECT(NumberField, TextField)
    public:
        NumberField()
        {
            SetTag(core::StringView(u8"numberfield"));
            NumberField* self = this;
            m_up = core::MakeRef<SpinButton>(MemoryAllocator(), true);
            m_down = core::MakeRef<SpinButton>(MemoryAllocator(), false);
            m_up->SetOnClick([self]() { self->StepBy(1.0); });
            m_down->SetOnClick([self]() { self->StepBy(-1.0); });
            AddChild(m_up.Get());
            AddChild(m_down.Get());
            m_range.Max = 100.0;
            m_range.Step = 1.0;
            ShowValue();
        }

        // === Value ===
        /// Returns the value. A binding that reads it follows later changes.
        [[nodiscard]] f64 GetValue() const
        {
            m_valueChanged.Track();
            return m_range.Value;
        }
        /// Sets the value from code. It is constrained, shown and reported to bindings, with no events.
        void SetValue(f64 value)
        {
            m_mixed = false;
            m_commitAlways = false;
            if (Apply(value))
                m_committedValue = m_range.Value;
            ShowValue();
        }
        [[nodiscard]] f64 GetMin() const noexcept { return m_range.Min; }
        void SetMin(f64 min) { EditRange([&](RangeValue& r) { r.Min = min; }); }
        [[nodiscard]] f64 GetMax() const noexcept { return m_range.Max; }
        void SetMax(f64 max) { EditRange([&](RangeValue& r) { r.Max = max; }); }
        [[nodiscard]] f64 GetStep() const noexcept { return m_range.Step; }
        void SetStep(f64 step) { EditRange([&](RangeValue& r) { r.Step = step < 0.0 ? 0.0 : step; }); }
        /// Returns the step for Page Up and Page Down; 0 means a tenth of the range.
        [[nodiscard]] f64 GetPage() const noexcept { return m_range.Page; }
        void SetPage(f64 page) { EditRange([&](RangeValue& r) { r.Page = page < 0.0 ? 0.0 : page; }); }
        [[nodiscard]] bool AllowsGreater() const noexcept { return m_range.AllowGreater; }
        void SetAllowGreater(bool allow) { EditRange([&](RangeValue& r) { r.AllowGreater = allow; }); }
        [[nodiscard]] bool AllowsLesser() const noexcept { return m_range.AllowLesser; }
        void SetAllowLesser(bool allow) { EditRange([&](RangeValue& r) { r.AllowLesser = allow; }); }
        /// Whether the field holds whole numbers only, snapping to them and writing no decimals.
        [[nodiscard]] bool IsInteger() const noexcept { return m_integer; }
        void SetInteger(bool integer)
        {
            m_integer = integer;
            SetValue(m_range.Value);
        }
        [[nodiscard]] const RangeValue& GetRange() const noexcept { return m_range; }

        /// Sets the text before the number (such as "$"); SetSuffix sets the text after it (" px").
        void SetPrefix(const core::String& prefix)
        {
            m_prefix = prefix;
            ShowValue();
        }
        [[nodiscard]] core::String GetPrefix() const { return m_prefix; }
        void SetSuffix(const core::String& suffix)
        {
            m_suffix = suffix;
            ShowValue();
        }
        [[nodiscard]] core::String GetSuffix() const { return m_suffix; }

        // Markup sets the prefix and suffix exactly as written, so a suffix's leading space is kept.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"prefix"))
                SetPrefix(core::String(value));
            else if (name == core::StringView(u8"suffix"))
                SetSuffix(core::String(value));
            else
                return TextField::SetMarkupAttribute(name, value);
            return true;
        }

        /// Sets the callback for each change the user makes as it happens, such as each step of a
        /// scrub.
        void SetOnValueChanging(core::Function<void(f64)> callback) { m_onChanging = core::Move(callback); }
        /// Sets the callback for a change the user committed: a typed value, a step, or a scrub's
        /// release.
        void SetOnValueChanged(core::Function<void(f64)> callback) { m_onChanged = core::Move(callback); }

        /// Sets the mixed state, in which no value shows because the values it stands for differ.
        /// Call this after SetValue, which clears it.
        void SetMixed(bool mixed)
        {
            if (m_mixed == mixed)
                return;
            m_mixed = mixed;
            m_commitAlways = false;
            m_committedValue = m_range.Value;
            ShowValue();
        }
        [[nodiscard]] bool IsMixed() const noexcept { return m_mixed; }
        /// Sets whether the wheel steps the value while the field is focused. When off, the wheel
        /// event passes on to whatever scrolls.
        void SetStepsOnWheel(bool steps) noexcept { m_stepsOnWheel = steps; }
        [[nodiscard]] bool StepsOnWheel() const noexcept { return m_stepsOnWheel; }
        /// Sets the callback for a scrub cancelled with Escape, after the value is restored.
        void SetOnValueCancelled(core::Function<void()> callback) { m_onCancelled = core::Move(callback); }

        /// Returns whether the field is being edited as text (typed into, or clicked). When it is
        /// not, a press scrubs.
        [[nodiscard]] bool IsEditing() const noexcept { return m_editing; }
        /// Returns whether the arrows show, which they do only while the number still has room
        /// beside them. A field too narrow for both (one component of a vector in a narrow
        /// inspector) gives the arrows' room to the number; dragging still scrubs it and the arrow
        /// keys still step it.
        [[nodiscard]] bool ShowsArrows() const
        {
            const Thickness b = GetBorderThickness();
            const Thickness p = GetPadding();
            const f32 text = GetSize().x - b.TotalHorizontal() - p.TotalHorizontal() - kSpinWidth - kSpinGap;
            return text >= kNumberRoom;
        }

        [[nodiscard]] SpinButton* GetUpButton() const noexcept { return m_up.Get(); }
        [[nodiscard]] SpinButton* GetDownButton() const noexcept { return m_down.Get(); }

        /// Formats `value` as the field writes it: with the step's decimals, between the prefix and
        /// the suffix.
        [[nodiscard]] core::String FormatValue(f64 value) const
        {
            const i32 decimals = m_integer ? 0 : m_range.Decimals();
            f64 scale = 1.0;
            for (i32 d = 0; d < decimals; ++d)
                scale *= 10.0;
            const f64 rounded = static_cast<f64>(static_cast<i64>(value * scale + (value < 0.0 ? -0.5 : 0.5))) / scale;
            core::String text(m_prefix);
            if (decimals == 0)
                text += core::Format(u8"{}", static_cast<i64>(rounded)).AsView();
            else
                text += core::Format(u8"{}", rounded).AsView();
            text += m_suffix.AsView();
            return text;
        }

    protected:
        // Reads the text as an expression. A valid number is committed; otherwise the old value is
        // shown again.
        void OnCommitting() override
        {
            core::StringView text = Trim(GetModel().GetText());
            if (m_prefix.Size() != 0 && text.Size() >= m_prefix.Size() && text.SubStr(0, m_prefix.Size()) == m_prefix.AsView())
                text = Trim(text.SubStr(m_prefix.Size(), text.Size() - m_prefix.Size()));
            if (m_suffix.Size() != 0 && text.Size() >= m_suffix.Size() &&
                text.SubStr(text.Size() - m_suffix.Size(), m_suffix.Size()) == m_suffix.AsView())
                text = Trim(text.SubStr(0, text.Size() - m_suffix.Size()));
            if (const Optional<f64> v = EvaluateExpression(text); v.HasValue())
                ChangeByUser(v.Value(), false);
            else if (m_mixed)
                return; // nothing valid was typed into a mixed field, so it stays mixed
            ShowValue();
            CommitValue();
        }

        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Target == this && event.Modifiers == 0 && IsTreeEnabled())
            {
                const KeyCode key = static_cast<KeyCode>(event.KeyCode);
                if (key == KeyCode::Escape && m_pressing)
                {
                    CancelScrub();
                    event.Handled = true;
                    return;
                }
                f64 by = 0.0;
                if (key == KeyCode::Up)
                    by = m_range.KeyStep();
                else if (key == KeyCode::Down)
                    by = -m_range.KeyStep();
                else if (key == KeyCode::PageUp)
                    by = m_range.PageStep();
                else if (key == KeyCode::PageDown)
                    by = -m_range.PageStep();
                if (by != 0.0)
                {
                    StepValue(by);
                    event.Handled = true;
                    return;
                }
                if (key == KeyCode::Space || (key >= KeyCode::A && key <= KeyCode::Num9))
                    m_editing = true; // the user is typing into it
            }
            TextField::OnKeyDown(event);
        }
        void OnTextInput(const TextInputEvent& event) override
        {
            m_editing = true;
            TextField::OnTextInput(event);
        }
        void OnMouseWheel(const WheelEvent& event) override
        {
            if (!m_stepsOnWheel || !IsFocused() || event.Delta.y == 0.0f)
                return;
            StepValue(m_range.KeyStep() * static_cast<f64>(event.Delta.y));
            event.Handled = true;
        }

        void OnMouseDown(const MouseEvent& event) override
        {
            if (m_editing || event.Button != MouseButton::Left)
            {
                TextField::OnMouseDown(event);
                return;
            }
            // Not editing: the press may become a scrub, or a click that starts editing.
            RequestFocus();
            event.Handled = true;
            m_pressing = true;
            m_scrubbing = false;
            m_pressX = event.Position.x;
            m_scrubFrom = m_range.Value;
            m_scrubFromMixed = m_mixed;
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_pressing)
            {
                TextField::OnMouseMove(event);
                return;
            }
            // Dragging toward the inline end raises the value, as with a slider.
            const f32 dx = (event.Position.x - m_pressX) * (IsRightToLeft() ? -1.0f : 1.0f);
            if (!m_scrubbing && core::Abs(dx) < kScrubThreshold)
                return;
            m_scrubbing = true;
            // One step per few pixels; Shift goes a tenth as fast, Ctrl ten times as fast.
            f64 rate = (m_range.Step > 0.0 ? m_range.Step : (m_range.Max - m_range.Min) / 200.0) / kPixelsPerStep;
            if ((event.Modifiers & KeyModShift) != 0)
                rate *= 0.1;
            if ((event.Modifiers & KeyModCtrl) != 0)
                rate *= 10.0;
            ChangeByUser(m_scrubFrom + static_cast<f64>(dx) * rate, true);
            ShowValue();
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            if (!m_pressing)
            {
                TextField::OnMouseUp(event);
                return;
            }
            m_pressing = false;
            if (m_scrubbing)
            {
                m_scrubbing = false;
                CommitValue();
                return;
            }
            // A click starts editing with all the text selected.
            m_editing = true;
            GetModel().SelectAll();
            Synced();
        }
        void OnPointerCancel(const MouseEvent& event) override
        {
            if (m_scrubbing)
                CommitValue();
            m_pressing = false;
            m_scrubbing = false;
            TextField::OnPointerCancel(event);
        }
        void OnFocusLost() override
        {
            TextField::OnFocusLost(); // commits
            m_editing = false;
        }

        // Keeps the text clear of the arrows at the inline end.
        [[nodiscard]] Rect EditBounds() const override
        {
            const Rect content = GetContentBounds();
            const Thickness b = GetBorderThickness();
            if (!ShowsArrows())
                return content;
            if (IsRightToLeft())
            {
                const f32 left = core::Max(content.x, b.Left + kSpinWidth + kSpinGap);
                return Rect{left, content.y, core::Max(0.0f, content.Right() - left), content.height};
            }
            const f32 right = core::Min(content.Right(), GetSize().x - b.Right - kSpinWidth - kSpinGap);
            return Rect{content.x, content.y, core::Max(0.0f, right - content.x), content.height};
        }

        // Stacks the arrows at the inline end, inside the border.
        void OnLayout() override
        {
            const Thickness b = GetBorderThickness();
            const core::Float2 size = GetSize();
            const f32 h = core::Max(0.0f, size.y - b.TotalVertical());
            const f32 x = IsRightToLeft() ? b.Left : size.x - b.Right - kSpinWidth;
            const bool arrows = ShowsArrows();
            m_up->SetVisible(arrows);
            m_down->SetVisible(arrows);
            m_up->Arrange(Rect{x, b.Top, kSpinWidth, h * 0.5f});
            m_down->Arrange(Rect{x, b.Top + h * 0.5f, kSpinWidth, h - h * 0.5f});
        }

    private:
        static constexpr f32 kSpinWidth = 16.0f;
        static constexpr f32 kSpinGap = 2.0f; // gap between the text and the arrows
        static constexpr f32 kNumberRoom = 32.0f; // the least room the number needs beside the arrows
        static constexpr f32 kScrubThreshold = 4.0f; // pixels a press must move before it scrubs
        static constexpr f32 kPixelsPerStep = 4.0f;

        bool Apply(f64 value)
        {
            if (m_integer)
                value = static_cast<f64>(static_cast<i64>(value + (value < 0.0 ? -0.5 : 0.5)));
            value = m_range.Constrain(value);
            if (value == m_range.Value && !m_mixed)
                return false;
            m_mixed = false; // the user set a single value, whatever it is
            m_range.Value = value;
            m_valueChanged.Notify();
            return true;
        }
        template <typename F>
        void EditRange(F&& change)
        {
            change(m_range);
            const f64 before = m_range.Value;
            m_range.Value = m_range.Constrain(m_range.Value);
            if (m_range.Value != before)
            {
                m_committedValue = m_range.Value;
                m_valueChanged.Notify();
            }
            ShowValue();
        }
        // Applies a change made by the user. With `reportInput` it sends `input`; CommitValue sends
        // `change`, now or at the end of the gesture.
        void ChangeByUser(f64 value, bool reportInput)
        {
            const bool wasMixed = m_mixed;
            if (!Apply(value))
                return;
            if (wasMixed)
                m_commitAlways = true; // commit even if the value equals the one it held
            if (reportInput)
            {
                if (m_onChanging)
                    m_onChanging(m_range.Value);
                SendEvent(Event(EventType::Input, this));
            }
        }
        void CommitValue()
        {
            if (m_range.Value == m_committedValue && !m_commitAlways)
                return;
            m_commitAlways = false;
            m_committedValue = m_range.Value;
            if (m_onChanged)
                m_onChanged(m_range.Value);
            SendEvent(Event(EventType::Changed, this));
        }
        // Steps from an arrow, a key or the wheel. The step commits at once and any typing is dropped.
        void StepBy(f64 steps) { StepValue(m_range.KeyStep() * steps); }
        void StepValue(f64 by)
        {
            if (!IsTreeEnabled())
                return;
            ChangeByUser(m_range.Value + by, true);
            m_editing = false;
            ShowValue();
            CommitValue();
        }
        // Shows the value as the text, which also becomes the committed text so the field reports no
        // edit. In the mixed state the text is empty and the placeholder shows.
        void ShowValue() { SetText(m_mixed ? core::StringView{} : FormatValue(m_range.Value).AsView()); }
        // Handles Escape during a scrub: restores the value and the mixed state, ends the press and
        // reports the cancel.
        void CancelScrub()
        {
            const bool scrubbed = m_scrubbing;
            m_pressing = false;
            m_scrubbing = false;
            if (EventDispatcher* dispatcher = GetEventDispatcher())
                dispatcher->ReleasePointer(*this);
            if (!scrubbed)
                return;
            Apply(m_scrubFrom);
            m_mixed = m_scrubFromMixed;
            m_commitAlways = false;
            m_committedValue = m_range.Value;
            ShowValue();
            if (m_onCancelled)
                m_onCancelled();
        }

        RangeValue m_range;
        f64 m_committedValue = 0.0;
        bool m_integer = false;
        bool m_editing = false;
        bool m_pressing = false;  // a press that has not yet become a click or a scrub
        bool m_scrubbing = false;
        bool m_mixed = false;
        bool m_scrubFromMixed = false;
        bool m_commitAlways = false; // a mixed field's first change commits whatever the value
        bool m_stepsOnWheel = true;
        f32 m_pressX = 0.0f;
        f64 m_scrubFrom = 0.0;
        core::String m_prefix;
        core::String m_suffix;
        Observable m_valueChanged;
        core::RefPtr<SpinButton> m_up;
        core::RefPtr<SpinButton> m_down;
        core::Function<void(f64)> m_onChanging;
        core::Function<void(f64)> m_onChanged;
        core::Function<void()> m_onCancelled;
    };
}
