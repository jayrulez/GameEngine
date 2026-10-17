// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :actions partition
//
// Concrete actions that animate a Node: Move / Fade / Scale (interpolated), Delay,
// Runnable (a deferred callback), and Sequence (run children in order). Derived from
// eepp's scene/actions/*. These live here (not in :action) because they dereference the
// target Node, so this partition imports the full :node.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:actions;

import foundation.core; // Float2, Duration, Lerp, Function, RefPtr, Array, Move, Color, MakeRef
import :action;
import :node;
import :node_box;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    // Interpolates the target's position from `from` to `to`.
    class MoveAction : public ActionInterpolation
    {
        RTTI_OBJECT(MoveAction, ActionInterpolation)
    public:
        MoveAction(core::Float2 from, core::Float2 to, core::Duration duration) noexcept
            : ActionInterpolation(duration), m_from(from), m_to(to)
        {
        }

    protected:
        void OnStep(f32 t) override
        {
            if (m_target)
                m_target->SetPosition(core::Lerp(m_from, m_to, t));
        }

    private:
        core::Float2 m_from, m_to;
    };
    RTTI_DEFINE_OBJECT(MoveAction, "rtti::gui")

    // Interpolates the target's alpha from `from` to `to`.
    class FadeAction : public ActionInterpolation
    {
        RTTI_OBJECT(FadeAction, ActionInterpolation)
    public:
        FadeAction(f32 from, f32 to, core::Duration duration) noexcept
            : ActionInterpolation(duration), m_from(from), m_to(to)
        {
        }

        [[nodiscard]] bool CanMoveTarget() const override { return false; }

    protected:
        void OnStep(f32 t) override
        {
            if (m_target)
                m_target->SetAlpha(m_from + (m_to - m_from) * t);
        }

    private:
        f32 m_from, m_to;
    };
    RTTI_DEFINE_OBJECT(FadeAction, "rtti::gui")

    // Interpolates the target's scale from `from` to `to`.
    class ScaleAction : public ActionInterpolation
    {
        RTTI_OBJECT(ScaleAction, ActionInterpolation)
    public:
        ScaleAction(core::Float2 from, core::Float2 to, core::Duration duration) noexcept
            : ActionInterpolation(duration), m_from(from), m_to(to)
        {
        }

    protected:
        void OnStep(f32 t) override
        {
            if (m_target)
                m_target->SetScale(core::Lerp(m_from, m_to, t));
        }

    private:
        core::Float2 m_from, m_to;
    };
    RTTI_DEFINE_OBJECT(ScaleAction, "rtti::gui")

    // Waits a duration, then completes (no visible effect).
    class DelayAction : public ActionInterpolation
    {
        RTTI_OBJECT(DelayAction, ActionInterpolation)
    public:
        explicit DelayAction(core::Duration duration) noexcept : ActionInterpolation(duration) {}
        [[nodiscard]] bool CanMoveTarget() const override { return false; }

    protected:
        void OnStep(f32) override {}
    };
    RTTI_DEFINE_OBJECT(DelayAction, "rtti::gui")

    // Runs a callback once, after an optional delay.
    class RunnableAction : public ActionInterpolation
    {
        RTTI_OBJECT(RunnableAction, ActionInterpolation)
    public:
        explicit RunnableAction(core::Function<void()> fn,
                                core::Duration delay = core::Duration{}) noexcept
            : ActionInterpolation(delay), m_fn(core::Move(fn))
        {
        }

    protected:
        void OnStep(f32 t) override
        {
            if (t >= 1.0f && !m_ran && m_fn)
            {
                m_ran = true;
                m_fn();
            }
        }

    private:
        core::Function<void()> m_fn;
        bool m_ran = false;
    };
    RTTI_DEFINE_OBJECT(RunnableAction, "rtti::gui")

    // One color keyframe stop (offset in [0,1] + a color), for animating background-color.
    struct ColorKey
    {
        f32 Offset = 0.0f;
        Color Value;
    };

    // Drives a CSS @keyframes animation: interpolates the target's animatable properties across
    // the keyframe stops over `duration`, optionally looping forever. Supported tracks: opacity
    // ({offset, opacity} pairs -> SetAlpha) and background-color (ColorKeys -> SetBackground).
    // Empty tracks are skipped. Transform/size follow the same shape once they are wired.
    class KeyframeAction : public Action
    {
        RTTI_OBJECT(KeyframeAction, Action)
    public:
        KeyframeAction(Array<core::Float2> opacityTrack, Array<ColorKey> colorTrack,
                       core::Duration duration, bool loop) noexcept
            : m_opacity(core::Move(opacityTrack)), m_color(core::Move(colorTrack)),
              m_duration(duration), m_loop(loop)
        {
        }

        [[nodiscard]] bool CanMoveTarget() const override { return false; } // opacity and colour

        void Start() override
        {
            m_elapsed = core::Duration{};
            m_done = false;
            Apply(0.0f);
        }
        void Stop() override { m_done = true; }

        void Update(core::Duration elapsed) override
        {
            if (m_done)
                return;
            m_elapsed += elapsed;
            const f32 total = m_duration.AsSecondsF();
            if (total <= 0.0f)
            {
                Apply(1.0f);
                m_done = true;
                FireDone();
                return;
            }

            const f32 cycles = m_elapsed.AsSecondsF() / total;
            if (!m_loop && cycles >= 1.0f)
            {
                Apply(1.0f);
                m_done = true;
                FireDone();
                return;
            }
            const f32 t = m_loop ? (cycles - static_cast<f32>(static_cast<i64>(cycles)))
                                 : core::Min(cycles, 1.0f);
            Apply(t);
        }

        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override
        {
            return m_duration.AsSecondsF() > 0.0f
                       ? core::Min(m_elapsed.AsSecondsF() / m_duration.AsSecondsF(), 1.0f)
                       : 1.0f;
        }
        [[nodiscard]] core::Duration GetTotalTime() override { return m_duration; }

    private:
        void Apply(f32 t)
        {
            if (m_target == nullptr)
                return;
            if (m_opacity.Size() != 0)
                m_target->SetAlpha(OpacityAt(t));
            if (m_color.Size() != 0)
            {
                NodeBox box = m_target->GetBox();
                box.BackgroundColor = ColorAt(t);
                m_target->SetBox(core::Move(box), m_target->IsBoxFromStyle());
            }
        }

        [[nodiscard]] f32 OpacityAt(f32 t) const
        {
            if (t <= m_opacity[0].x)
                return m_opacity[0].y;
            for (usize i = 1; i < m_opacity.Size(); ++i)
                if (t <= m_opacity[i].x)
                {
                    const f32 span = m_opacity[i].x - m_opacity[i - 1].x;
                    const f32 lt = span > 0.0f ? (t - m_opacity[i - 1].x) / span : 0.0f;
                    return m_opacity[i - 1].y + (m_opacity[i].y - m_opacity[i - 1].y) * lt;
                }
            return m_opacity[m_opacity.Size() - 1].y;
        }

        [[nodiscard]] Color ColorAt(f32 t) const
        {
            if (t <= m_color[0].Offset)
                return m_color[0].Value;
            for (usize i = 1; i < m_color.Size(); ++i)
                if (t <= m_color[i].Offset)
                {
                    const f32 span = m_color[i].Offset - m_color[i - 1].Offset;
                    const f32 lt = span > 0.0f ? (t - m_color[i - 1].Offset) / span : 0.0f;
                    return LerpColor(m_color[i - 1].Value, m_color[i].Value, lt);
                }
            return m_color[m_color.Size() - 1].Value;
        }
        [[nodiscard]] static Color LerpColor(Color a, Color b, f32 t)
        {
            return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                         a.a + (b.a - a.a) * t};
        }

        Array<core::Float2> m_opacity; // {offset, opacity}
        Array<ColorKey> m_color;       // background-color stops
        core::Duration m_duration;
        core::Duration m_elapsed;
        bool m_loop = false;
        bool m_done = false;
    };
    RTTI_DEFINE_OBJECT(KeyframeAction, "rtti::gui")

    // Runs child actions one after another; done when the last finishes.
    class SequenceAction : public Action
    {
        RTTI_OBJECT(SequenceAction, Action)
    public:
        void Add(RefPtr<Action> action) { m_children.PushBack(core::Move(action)); }
        [[nodiscard]] bool CanMoveTarget() const override
        {
            for (const RefPtr<Action>& c : m_children)
                if (c->CanMoveTarget())
                    return true;
            return false;
        }

        void Start() override
        {
            m_index = 0;
            m_done = m_children.Size() == 0;
            if (!m_done)
                StartCurrent();
        }
        void Stop() override { m_done = true; }

        void Update(core::Duration elapsed) override
        {
            if (m_done || m_index >= m_children.Size())
                return;
            Action* current = m_children[m_index].Get();
            current->Update(elapsed);
            if (current->IsDone())
            {
                ++m_index;
                if (m_index >= m_children.Size())
                {
                    m_done = true;
                    FireDone();
                }
                else
                    StartCurrent();
            }
        }

        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override
        {
            return m_children.Size()
                       ? static_cast<f32>(m_index) / static_cast<f32>(m_children.Size())
                       : 1.0f;
        }
        [[nodiscard]] core::Duration GetTotalTime() override
        {
            core::Duration total{};
            for (const RefPtr<Action>& c : m_children)
                total += c->GetTotalTime();
            return total;
        }

    protected:
        void OnTargetChange() override
        {
            for (const RefPtr<Action>& c : m_children)
                c->SetTarget(m_target);
        }

    private:
        void StartCurrent()
        {
            if (m_index < m_children.Size())
            {
                m_children[m_index]->SetTarget(m_target);
                m_children[m_index]->Start();
            }
        }

        Array<RefPtr<Action>> m_children;
        usize m_index = 0;
        bool m_done = false;
    };
    RTTI_DEFINE_OBJECT(SequenceAction, "rtti::gui")

    // Calls a function with each frame's elapsed time on its target's clock until the function
    // returns false (or the action is stopped): a widget's own timing with nothing to animate, a
    // caret's blink.
    class TickAction : public Action
    {
        RTTI_OBJECT(TickAction, Action)
    public:
        using Tick = core::Function<bool(core::Duration)>;
        explicit TickAction(Tick tick) noexcept : m_tick(core::Move(tick)) {}

        void Start() override { m_done = false; }
        void Stop() override { m_done = true; }
        void Update(core::Duration elapsed) override
        {
            if (!m_done && (!m_tick || !m_tick(elapsed)))
            {
                m_done = true;
                FireDone();
            }
        }
        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override { return m_done ? 1.0f : 0.0f; }
        [[nodiscard]] core::Duration GetTotalTime() override { return core::Duration{}; }

    private:
        Tick m_tick;
        bool m_done = false;
    };
    RTTI_DEFINE_OBJECT(TickAction, "rtti::gui")
}
