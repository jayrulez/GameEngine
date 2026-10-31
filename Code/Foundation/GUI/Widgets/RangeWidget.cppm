// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :range_widget partition
//
// RangeWidget: the base that a slider, a scroll bar, a progress bar and a number field share. It
// holds a RangeValue and reflects its properties (value, min, max, step, exponential,
// allow-greater, allow-lesser). The page is protected: the controls where a page means something
// (a scroll bar's visible part, the page keys of a slider or a number field) expose it, and a
// progress bar does not. A value set from code sends no events, though bindings follow it. The
// user's changes send `input` as they happen and `change` when committed (on a drag's release, a
// key, or a typed value's commit), as the text fields do.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:range_widget;

import foundation.core; // Function, Move
import :reactive;       // Observable
import :event;
import :ui_widget;
import :range_value;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class RangeWidget : public UIWidget
    {
        RTTI_OBJECT(RangeWidget, UIWidget)
    public:
        /// Returns the value. A binding that reads it follows later changes.
        [[nodiscard]] f64 GetValue() const
        {
            m_valueChanged.Track();
            return m_range.Value;
        }
        /// Sets the value from code. It is constrained and reported to bindings, with no events.
        void SetValue(f64 value)
        {
            if (Apply(value))
                m_committed = m_range.Value;
        }

        [[nodiscard]] f64 GetMin() const noexcept { return m_range.Min; }
        void SetMin(f64 min) { Edit([&](RangeValue& r) { r.Min = min; }); }
        [[nodiscard]] f64 GetMax() const noexcept { return m_range.Max; }
        void SetMax(f64 max) { Edit([&](RangeValue& r) { r.Max = max; }); }
        [[nodiscard]] f64 GetStep() const noexcept { return m_range.Step; }
        void SetStep(f64 step) { Edit([&](RangeValue& r) { r.Step = step < 0.0 ? 0.0 : step; }); }
        [[nodiscard]] bool IsExponential() const noexcept { return m_range.Exponential; }
        void SetExponential(bool exponential) { Edit([&](RangeValue& r) { r.Exponential = exponential; }); }
        [[nodiscard]] bool AllowsGreater() const noexcept { return m_range.AllowGreater; }
        void SetAllowGreater(bool allow) { Edit([&](RangeValue& r) { r.AllowGreater = allow; }); }
        [[nodiscard]] bool AllowsLesser() const noexcept { return m_range.AllowLesser; }
        void SetAllowLesser(bool allow) { Edit([&](RangeValue& r) { r.AllowLesser = allow; }); }
        [[nodiscard]] const RangeValue& GetRange() const noexcept { return m_range; }
        /// Returns where the value lies along the range, from 0 to 1.
        [[nodiscard]] f64 GetRatio() const { return m_range.Ratio(); }

        /// Sets the callback for each change the user makes as it happens (the `input` event).
        void SetOnValueChanging(core::Function<void(f64)> callback) { m_onChanging = core::Move(callback); }
        /// Sets the callback for a change the user committed (the `change` event).
        void SetOnValueChanged(core::Function<void(f64)> callback) { m_onChanged = core::Move(callback); }

    protected:
        RangeWidget() = default;

        [[nodiscard]] f64 GetPageSize() const noexcept { return m_range.Page; }
        /// Sets whether the page is a visible part, as in a scroll bar; the value then stops short
        /// of the maximum by the page.
        void SetPageShown(bool shown) { Edit([&](RangeValue& r) { r.PageShown = shown; }); }
        void SetPageSize(f64 page) { Edit([&](RangeValue& r) { r.Page = page < 0.0 ? 0.0 : page; }); }

        /// Applies a user change to `value`. It sends `input`, and commits (`change`) now when
        /// `commit` is set, or later through Commit.
        void ChangeByUser(f64 value, bool commit)
        {
            if (Apply(value))
            {
                if (m_onChanging)
                    m_onChanging(m_range.Value);
                SendEvent(Event(EventType::Input, this));
            }
            if (commit)
                Commit();
        }
        /// Called when a gesture ends (a drag is released); commits its change, if any.
        void Commit()
        {
            if (m_range.Value == m_committed)
                return;
            m_committed = m_range.Value;
            if (m_onChanged)
                m_onChanged(m_range.Value);
            SendEvent(Event(EventType::Changed, this));
        }
        /// Called when the range or the value changed. Redraws by default; a control with parts of
        /// its own does more.
        virtual void OnRangeChange() { Invalidate(); }

    private:
        bool Apply(f64 value)
        {
            value = m_range.Constrain(value);
            if (value == m_range.Value)
                return false;
            m_range.Value = value;
            OnRangeChange();
            m_valueChanged.Notify();
            return true;
        }
        template <typename F>
        void Edit(F&& change)
        {
            RangeValue next = m_range;
            change(next);
            next.Value = next.Constrain(next.Value);
            if (next == m_range)
                return;
            const bool moved = next.Value != m_range.Value;
            m_range = next;
            OnRangeChange();
            if (moved)
            {
                m_committed = m_range.Value;
                m_valueChanged.Notify();
            }
        }

        RangeValue m_range;
        f64 m_committed = 0.0; // the value last committed, so each change reports once
        Observable m_valueChanged;
        core::Function<void(f64)> m_onChanging;
        core::Function<void(f64)> m_onChanged;
    };
}
