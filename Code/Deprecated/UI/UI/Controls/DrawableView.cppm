// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :drawable_view partition
//
// View that renders any Drawable at a given size. Uses DesiredWidth/DesiredHeight if set, else the
// drawable's IntrinsicSize, else 0. Ported from Sedulous.UI/src/Controls/DrawableView.bf. The Beef
// `Drawable Drawable` + `OwnsDrawable` + `~this ReleaseRef` become a RefPtr<Drawable> (RAII - the flag
// is unnecessary). The `Drawable` field name shadows the Drawable type, so the RefPtr uses an alias.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:drawable_view;

import foundation.core;
import :view;
import :property;
import :control_state;
import :box_constraints;
import :draw_context;
import :drawable;

using namespace foundation::core;

export namespace foundation::ui
{
    // Alias so the faithful `Drawable` field name can still name the Drawable type (RefPtr element).
    using DrawablePtr = RefPtr<::foundation::ui::Drawable>;

    class DrawableView : public View
    {
        RTTI_OBJECT(DrawableView, View)
    public:
        /// The drawable to render (RefPtr-owned; shared).
        DrawablePtr Drawable;
        Property<Optional<f32>> DesiredWidth;
        Property<Optional<f32>> DesiredHeight;

        DrawableView()
        {
            DesiredWidth.SetOwner(this);
            DesiredHeight.SetOwner(this);
        }
        explicit DrawableView(DrawablePtr drawable) : DrawableView() { Drawable = Move(drawable); }
        DrawableView(DrawablePtr drawable, f32 width, f32 height) : DrawableView()
        {
            Drawable = Move(drawable);
            DesiredWidth.SetSilent(Optional<f32>{width});
            DesiredHeight.SetSilent(Optional<f32>{height});
        }

        /// Draws at the desired (else intrinsic) aspect, as large as fits and centred, rather than
        /// stretched to the view's box: an icon in a squeezed row shrinks, it does not squash.
        bool KeepAspect = false;

        void OnDraw(UIDrawContext& ctx) override
        {
            if (!Drawable)
            {
                return;
            }
            Rectangle rect{0, 0, Width(), Height()};
            if (KeepAspect)
            {
                const Optional<Float2> intrinsic = Drawable->IntrinsicSize();
                const f32 aw = DesiredWidth.Value().HasValue()
                                   ? DesiredWidth.Value().Value()
                                   : (intrinsic.HasValue() ? intrinsic.Value().x : 0.0f);
                const f32 ah = DesiredHeight.Value().HasValue()
                                   ? DesiredHeight.Value().Value()
                                   : (intrinsic.HasValue() ? intrinsic.Value().y : 0.0f);
                if (aw > 0.0f && ah > 0.0f && Width() > 0.0f && Height() > 0.0f)
                {
                    const f32 scale = Min(Width() / aw, Height() / ah);
                    const f32 w = aw * scale;
                    const f32 h = ah * scale;
                    rect = Rectangle{(Width() - w) * 0.5f, (Height() - h) * 0.5f, w, h};
                }
            }
            Drawable->Draw(ctx, rect, GetControlState());
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            Optional<Float2> intrinsic = Drawable ? Drawable->IntrinsicSize() : Optional<Float2>{};
            const f32 w = DesiredWidth.Value().HasValue()
                              ? DesiredWidth.Value().Value()
                              : (intrinsic.HasValue() ? intrinsic.Value().x : 0.0f);
            const f32 h = DesiredHeight.Value().HasValue()
                              ? DesiredHeight.Value().Value()
                              : (intrinsic.HasValue() ? intrinsic.Value().y : 0.0f);
            MeasuredSize = Float2{constraints.ConstrainWidth(w), constraints.ConstrainHeight(h)};
        }
    };

    RTTI_DEFINE_OBJECT(DrawableView, "rtti::ui")
}
