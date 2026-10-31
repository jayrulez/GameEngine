// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :state_list_drawable partition
//
// Maps ControlState flags to Drawables with fallback lookup: the exact flag combination,
// else the weightiest defined state made of its flags, else Normal (see Get).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:state_list_drawable;

import foundation.core; // HashMap, RefPtr, Rectangle
import :control_state;
import :drawable;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class StateListDrawable : public Drawable
    {
        RTTI_OBJECT(StateListDrawable, Drawable)
    public:
        StateListDrawable() = default;

        /// Set the drawable for a state (or flag combination); replaces and releases any prior one.
        void Set(ControlState state, RefPtr<Drawable> drawable)
        {
            m_drawables.InsertOrAssign(static_cast<u32>(state), drawable);
        }

        /// Get the drawable for a state: the exact match, else the defined state within it that
        /// says the most (each flag weighed: Disabled, then Pressed, then Checked/Indeterminate,
        /// then Focused, then Hover), else Normal. So a pressed control under the pointer
        /// (Pressed|Hover, rarely defined) draws pressed, not hover. Disabled dominates: with
        /// interaction flags set it takes its own drawable first, so a disabled control never
        /// lights up under the mouse and looks clickable.
        [[nodiscard]] Drawable* Get(ControlState state) const
        {
            const u32 key = static_cast<u32>(state);
            if (const RefPtr<Drawable>* exact = m_drawables.Find(key))
            {
                return exact->Get();
            }

            constexpr u32 kDisabled = static_cast<u32>(ControlState::Disabled);
            constexpr u32 kInteraction = static_cast<u32>(ControlState::Hover) |
                                         static_cast<u32>(ControlState::Pressed) |
                                         static_cast<u32>(ControlState::Focused);
            if ((key & kDisabled) != 0u && (key & kInteraction) != 0u)
            {
                if (const RefPtr<Drawable>* d = m_drawables.Find(key & ~kInteraction))
                {
                    return d->Get();
                }
            }

            // Every defined state made of this one's flags (Normal aside); the weightiest wins.
            Drawable* best = nullptr;
            u32 bestWeight = 0u;
            for (u32 subset = key; subset != 0u; subset = (subset - 1u) & key)
            {
                const RefPtr<Drawable>* candidate = m_drawables.Find(subset);
                if (candidate != nullptr && (best == nullptr || Weight(subset) > bestWeight))
                {
                    best = candidate->Get();
                    bestWeight = Weight(subset);
                }
            }
            if (best != nullptr)
            {
                return best;
            }
            if (const RefPtr<Drawable>* normal = m_drawables.Find(0u))
            {
                return normal->Get();
            }
            return nullptr;
        }

        /// How much a state's flags say, for choosing among partial matches.
        [[nodiscard]] static constexpr u32 Weight(u32 flags) noexcept
        {
            u32 weight = 0u;
            weight += (flags & static_cast<u32>(ControlState::Disabled)) != 0u ? 64u : 0u;
            weight += (flags & static_cast<u32>(ControlState::Pressed)) != 0u ? 32u : 0u;
            weight += (flags & static_cast<u32>(ControlState::Checked)) != 0u ? 16u : 0u;
            weight += (flags & static_cast<u32>(ControlState::Indeterminate)) != 0u ? 16u : 0u;
            weight += (flags & static_cast<u32>(ControlState::Focused)) != 0u ? 4u : 0u;
            weight += (flags & static_cast<u32>(ControlState::Hover)) != 0u ? 2u : 0u;
            return weight;
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (Drawable* d = Get(ControlState::Normal))
            {
                d->Draw(ctx, bounds);
            }
        }
        void DrawState(UIDrawContext& ctx, const Rectangle& bounds, ControlState state) override
        {
            if (Drawable* d = Get(state))
            {
                d->Draw(ctx, bounds);
            }
        }

        [[nodiscard]] Optional<vg::CornerRadii> OutlineRadii(ControlState state) const override
        {
            if (Drawable* d = Get(state))
            {
                return d->OutlineRadii(state);
            }
            return {};
        }

    private:
        HashMap<u32, RefPtr<Drawable>> m_drawables;
    };

    RTTI_DEFINE_OBJECT(StateListDrawable, "rtti::ui")
}
