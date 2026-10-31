// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :drawable partition
//
// Drawable: base class for composable visual primitives. Stateless; render into a bounds
// via UIDrawContext.
//
// Drawable derives `Object` (which is reference counted) so concrete drawables can be
// downcast with Cast<T> (the engine's RTTI; built with -fno-rtti). Drawables are not
// reflection-registered or scripted.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:drawable;

import foundation.core; // Object, Optional, Float2, Rectangle
import foundation.vg;   // CornerRadii
import :thickness;
import :control_state;
import :draw_context;

using namespace foundation::core;

export namespace foundation::ui
{
    class Drawable : public Object
    {
        RTTI_OBJECT(Drawable, Object)
    public:
        /// State-unaware draw.
        virtual void Draw(UIDrawContext& ctx, const Rectangle& bounds) = 0;

        /// State-aware draw: the one entry every control uses. Applies the draw context's
        /// transition blend (a background drawable swap cross-fades old under new; a control
        /// state change draws the old state under the new one at the eased opacity), then
        /// dispatches to DrawState. The blend is consumed here, so nested drawables (layers,
        /// insets) draw plainly.
        void Draw(UIDrawContext& ctx, const Rectangle& bounds, ControlState state)
        {
            const DrawBlend blend = ctx.Blend();
            if (!blend.IsActive())
            {
                DrawState(ctx, bounds, state);
                return;
            }
            const DrawBlend plain{};
            ctx.SetBlend(plain);
            if (blend.FromDrawable != nullptr && blend.ToDrawable == this && blend.FromDrawable != this)
            {
                // Old drawable at full strength, the new one fading in over it: opaque
                // backgrounds keep full coverage throughout (no mid-fade dip).
                ctx.SetBlend(DrawBlend{nullptr, nullptr, 1.0f, blend.StateActive, blend.FromState,
                                       blend.ToState, blend.StateT});
                blend.FromDrawable->Draw(ctx, bounds, state);
                ctx.SetBlend(DrawBlend{nullptr, nullptr, 1.0f, blend.StateActive, blend.FromState,
                                       blend.ToState, blend.StateT});
                ctx.VG().PushOpacity(blend.DrawableT);
                Draw(ctx, bounds, state);
                ctx.VG().PopOpacity();
            }
            else if (blend.StateActive && state == blend.ToState && blend.FromState != state)
            {
                DrawState(ctx, bounds, blend.FromState);
                ctx.VG().PushOpacity(blend.StateT);
                DrawState(ctx, bounds, state);
                ctx.VG().PopOpacity();
            }
            else
            {
                DrawState(ctx, bounds, state);
            }
            ctx.SetBlend(blend);
        }

        /// Optional natural size (e.g. icons/images). Empty = no intrinsic size.
        [[nodiscard]] virtual Optional<Float2> IntrinsicSize() const { return {}; }

        /// Padding contributed by this drawable (e.g. nine-slice borders). Layout can
        /// merge via max(drawablePadding, explicitPadding).
        [[nodiscard]] virtual Thickness DrawablePadding() const { return Thickness{}; }

        /// The corners of the box this drawable fills in `state`, when it fills a rounded box: what
        /// a box-shadow under it follows when the view sets no corner-radius. Empty for a drawable
        /// that is no such box (an image, a gradient, a custom shape).
        [[nodiscard]] virtual Optional<vg::CornerRadii> OutlineRadii(ControlState state) const
        {
            (void)state;
            return {};
        }

    protected:
        /// State-aware draw body; the default delegates to the state-unaware overload.
        /// Override this (not Draw) in drawables that pick by state.
        virtual void DrawState(UIDrawContext& ctx, const Rectangle& bounds, ControlState state)
        {
            (void)state;
            Draw(ctx, bounds);
        }
    };

    RTTI_DEFINE_OBJECT(Drawable, "rtti::ui")
}
