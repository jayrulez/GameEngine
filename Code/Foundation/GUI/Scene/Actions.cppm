// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :actions partition
//
// TickAction, the action a widget uses for its own timing: a function called each frame on the
// target's clock. Animating properties is done by the tweens (Styling/Tween.cppm).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:actions;

import foundation.core;
import :action;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    // Calls a function with each frame's elapsed time on its target's clock until the function
    // returns false or the action is stopped. A widget uses it for timing that animates no
    // property, such as a caret's blink.
    class TickAction : public Action
    {
        RTTI_OBJECT(TickAction, Action)
    public:
        using Tick = core::Function<bool(core::Duration)>;
        explicit TickAction(Tick tick) noexcept : m_tick(core::Move(tick)) {}
        /// A clock (`asksForFrames` false) runs every GUI update but asks for no frames itself, as
        /// for timers whose effects invalidate what they change.
        TickAction(Tick tick, bool asksForFrames) noexcept : m_tick(core::Move(tick)), m_asksForFrames(asksForFrames) {}

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
        [[nodiscard]] bool AsksForFrames() const override { return m_asksForFrames; }

    private:
        Tick m_tick;
        bool m_done = false;
        bool m_asksForFrames = true;
    };
    RTTI_DEFINE_OBJECT(TickAction, "rtti::gui")
}
