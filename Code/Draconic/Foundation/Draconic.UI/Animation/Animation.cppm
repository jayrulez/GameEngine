// Draconic UI - :animation partition
//
// Abstract base class for all property animations: manages elapsed time, easing, delay, repeat, and
// auto-reverse. Ported from Sedulous.UI/src/Animation/Animation.bf. Animations are single-owner (Beef
// `delete` -> UniquePtr held by AnimationManager / parent Storyboard); this base is a plain polymorphic
// class (not an Object - not scripted, no Cast). EasingFunction + the easing functions come from
// Draconic.Foundation (:easings). Target is a borrowed View* (forward-declared; only pointer-compared).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:animation;

import draconic.foundation;
import :event;

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;

    /// Abstract base class for all property animations.
    class Animation
    {
    public:
        Event<void(Animation*)> OnComplete;

        explicit Animation(f32 duration, EasingFunction easing = nullptr)
            : m_duration(Max(duration, 0.0f)), m_easing(easing)
        {
        }
        virtual ~Animation() = default;

        Animation(const Animation&) = delete;
        Animation& operator=(const Animation&) = delete;

        /// The view this animation targets (for AnimationManager.CancelForView).
        [[nodiscard]] View* Target() const noexcept { return m_target; }
        void SetTarget(View* value) noexcept { m_target = value; }

        /// Duration of one cycle in seconds.
        [[nodiscard]] f32 Duration() const noexcept { return m_duration; }
        void SetDuration(f32 value) noexcept { m_duration = Max(value, 0.0f); }

        /// Delay before the animation starts in seconds.
        [[nodiscard]] f32 Delay() const noexcept { return m_delay; }
        void SetDelay(f32 value) noexcept { m_delay = Max(value, 0.0f); }

        /// Easing function applied to progress. Null = linear.
        [[nodiscard]] EasingFunction Easing() const noexcept { return m_easing; }
        void SetEasing(EasingFunction value) noexcept { m_easing = value; }

        /// Whether the animation plays backward on alternate repeats.
        [[nodiscard]] bool AutoReverse() const noexcept { return m_autoReverse; }
        void SetAutoReverse(bool value) noexcept { m_autoReverse = value; }

        /// Number of times to repeat after the first play. 0 = once, -1 = infinite.
        [[nodiscard]] i32 RepeatCount() const noexcept { return m_repeatCount; }
        void SetRepeatCount(i32 value) noexcept { m_repeatCount = value; }

        [[nodiscard]] bool IsRunning() const noexcept { return m_isRunning; }
        [[nodiscard]] bool IsComplete() const noexcept { return m_isComplete; }
        [[nodiscard]] f32 Elapsed() const noexcept { return m_elapsed; }

        /// Start or resume the animation.
        void Start()
        {
            if (!m_isComplete)
            {
                m_isRunning = true;
            }
        }

        /// Pause the animation without resetting.
        void Stop() { m_isRunning = false; }

        /// Reset the animation to its initial state.
        virtual void Reset()
        {
            m_elapsed = 0;
            m_currentRepeat = 0;
            m_isRunning = false;
            m_isComplete = false;
        }

        /// Advance the animation by deltaTime. Returns true when fully complete.
        virtual bool Update(f32 deltaTime)
        {
            if (!m_isRunning || m_isComplete)
            {
                return m_isComplete;
            }

            m_elapsed += deltaTime;

            // Handle delay.
            if (m_delay > 0 && m_elapsed < m_delay)
            {
                return false;
            }

            const f32 activeTime = m_elapsed - m_delay;

            if (m_duration <= 0)
            {
                // Zero-duration: snap to end.
                Apply(1.0f);
                FinishCycle();
                return m_isComplete;
            }

            if (activeTime >= m_duration)
            {
                // Cycle complete.
                Apply(m_autoReverse && (m_currentRepeat & 1) != 0 ? 0.0f : 1.0f);
                FinishCycle();
                return m_isComplete;
            }

            // Normal progress.
            f32 t = activeTime / m_duration;

            // Auto-reverse: play backward on odd repeats.
            if (m_autoReverse && (m_currentRepeat & 1) != 0)
            {
                t = 1.0f - t;
            }

            // Apply easing.
            const f32 easedT = (m_easing != nullptr) ? m_easing(t) : t;
            Apply(easedT);

            return false;
        }

    protected:
        /// Apply the interpolated value at progress t (0-1, after easing).
        virtual void Apply(f32 t) = 0;

        /// Mark the animation as complete. For use by subclasses that override Update.
        void MarkComplete()
        {
            m_isRunning = false;
            m_isComplete = true;
            OnComplete.Invoke(this);
        }

    private:
        void FinishCycle()
        {
            if (m_repeatCount == -1 || m_currentRepeat < m_repeatCount)
            {
                // Start next repeat.
                m_currentRepeat++;
                m_elapsed = m_delay;
            }
            else
            {
                MarkComplete();
            }
        }

        f32 m_elapsed = 0.0f;
        f32 m_duration = 0.0f;
        f32 m_delay = 0.0f;
        EasingFunction m_easing = nullptr;
        bool m_isRunning = false;
        bool m_isComplete = false;
        bool m_autoReverse = false;
        i32 m_repeatCount = 0; // 0 = play once, -1 = infinite
        i32 m_currentRepeat = 0;
        View* m_target = nullptr;
    };
}
