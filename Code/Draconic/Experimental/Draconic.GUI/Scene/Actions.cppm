// Draconic GUI - :actions partition
//
// Concrete actions that animate a Node: Move / Fade / Scale (interpolated), Delay,
// Runnable (a deferred callback), and Sequence (run children in order). Derived from
// eepp's scene/actions/*. These live here (not in :action) because they dereference the
// target Node, so this partition imports the full :node.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:actions;

import draconic.foundation; // Float2, Duration, Lerp, Function, RefPtr, Array, Move, Color, MakeRef
import :action;
import :node;
import :rectangle_drawable;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    // Interpolates the target's position from `from` to `to`.
    class MoveAction : public ActionInterpolation
    {
        DRACONIC_OBJECT(MoveAction, ActionInterpolation)
    public:
        MoveAction(foundation::Float2 from, foundation::Float2 to, foundation::Duration duration) noexcept
            : ActionInterpolation(duration), m_from(from), m_to(to)
        {
        }

    protected:
        void OnStep(f32 t) override
        {
            if (m_target)
                m_target->SetPosition(foundation::Lerp(m_from, m_to, t));
        }

    private:
        foundation::Float2 m_from, m_to;
    };
    DRACONIC_DEFINE_OBJECT(MoveAction, "draconic::gui")

    // Interpolates the target's alpha from `from` to `to`.
    class FadeAction : public ActionInterpolation
    {
        DRACONIC_OBJECT(FadeAction, ActionInterpolation)
    public:
        FadeAction(f32 from, f32 to, foundation::Duration duration) noexcept
            : ActionInterpolation(duration), m_from(from), m_to(to)
        {
        }

    protected:
        void OnStep(f32 t) override
        {
            if (m_target)
                m_target->SetAlpha(m_from + (m_to - m_from) * t);
        }

    private:
        f32 m_from, m_to;
    };
    DRACONIC_DEFINE_OBJECT(FadeAction, "draconic::gui")

    // Interpolates the target's scale from `from` to `to`.
    class ScaleAction : public ActionInterpolation
    {
        DRACONIC_OBJECT(ScaleAction, ActionInterpolation)
    public:
        ScaleAction(foundation::Float2 from, foundation::Float2 to, foundation::Duration duration) noexcept
            : ActionInterpolation(duration), m_from(from), m_to(to)
        {
        }

    protected:
        void OnStep(f32 t) override
        {
            if (m_target)
                m_target->SetScale(foundation::Lerp(m_from, m_to, t));
        }

    private:
        foundation::Float2 m_from, m_to;
    };
    DRACONIC_DEFINE_OBJECT(ScaleAction, "draconic::gui")

    // Waits a duration, then completes (no visible effect).
    class DelayAction : public ActionInterpolation
    {
        DRACONIC_OBJECT(DelayAction, ActionInterpolation)
    public:
        explicit DelayAction(foundation::Duration duration) noexcept : ActionInterpolation(duration) {}

    protected:
        void OnStep(f32) override {}
    };
    DRACONIC_DEFINE_OBJECT(DelayAction, "draconic::gui")

    // Runs a callback once, after an optional delay.
    class RunnableAction : public ActionInterpolation
    {
        DRACONIC_OBJECT(RunnableAction, ActionInterpolation)
    public:
        explicit RunnableAction(foundation::Function<void()> fn,
                                foundation::Duration delay = foundation::Duration{}) noexcept
            : ActionInterpolation(delay), m_fn(foundation::Move(fn))
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
        foundation::Function<void()> m_fn;
        bool m_ran = false;
    };
    DRACONIC_DEFINE_OBJECT(RunnableAction, "draconic::gui")

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
        DRACONIC_OBJECT(KeyframeAction, Action)
    public:
        KeyframeAction(Array<foundation::Float2> opacityTrack, Array<ColorKey> colorTrack,
                       foundation::Duration duration, bool loop) noexcept
            : m_opacity(foundation::Move(opacityTrack)), m_color(foundation::Move(colorTrack)),
              m_duration(duration), m_loop(loop)
        {
        }

        void Start() override
        {
            m_elapsed = foundation::Duration{};
            m_done = false;
            Apply(0.0f);
        }
        void Stop() override { m_done = true; }

        void Update(foundation::Duration elapsed) override
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
                                 : foundation::Min(cycles, 1.0f);
            Apply(t);
        }

        [[nodiscard]] bool IsDone() override { return m_done; }
        [[nodiscard]] f32 GetCurrentProgress() override
        {
            return m_duration.AsSecondsF() > 0.0f
                       ? foundation::Min(m_elapsed.AsSecondsF() / m_duration.AsSecondsF(), 1.0f)
                       : 1.0f;
        }
        [[nodiscard]] foundation::Duration GetTotalTime() override { return m_duration; }

    private:
        void Apply(f32 t)
        {
            if (m_target == nullptr)
                return;
            if (m_opacity.Size() != 0)
                m_target->SetAlpha(OpacityAt(t));
            if (m_color.Size() != 0)
                m_target->SetBackground(
                    foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), ColorAt(t)));
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

        Array<foundation::Float2> m_opacity; // {offset, opacity}
        Array<ColorKey> m_color;       // background-color stops
        foundation::Duration m_duration;
        foundation::Duration m_elapsed;
        bool m_loop = false;
        bool m_done = false;
    };
    DRACONIC_DEFINE_OBJECT(KeyframeAction, "draconic::gui")

    // Runs child actions one after another; done when the last finishes.
    class SequenceAction : public Action
    {
        DRACONIC_OBJECT(SequenceAction, Action)
    public:
        void Add(RefPtr<Action> action) { m_children.PushBack(foundation::Move(action)); }

        void Start() override
        {
            m_index = 0;
            m_done = m_children.Size() == 0;
            if (!m_done)
                StartCurrent();
        }
        void Stop() override { m_done = true; }

        void Update(foundation::Duration elapsed) override
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
        [[nodiscard]] foundation::Duration GetTotalTime() override
        {
            foundation::Duration total{};
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
    DRACONIC_DEFINE_OBJECT(SequenceAction, "draconic::gui")
}
