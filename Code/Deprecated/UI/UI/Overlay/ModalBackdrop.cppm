// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :modal_backdrop partition
//
// Semi-transparent backdrop drawn behind modal popups; blocks input to underlying content by consuming
// all mouse events. Ported from Sedulous.UI/src/Overlay/ModalBackdrop.bf.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:modal_backdrop;

import foundation.core;
import :view;
import :style_property;
import :draw_context;
import :event_args;

using namespace foundation::core;

export namespace foundation::ui
{
    // Alias so the faithful `Color` field name can still name the core Color type (field shadows type).
    using ColorValue = foundation::core::Color;

    class ModalBackdrop : public View
    {
        RTTI_OBJECT(ModalBackdrop, View)
    public:
        ColorValue Color{0.0f, 0.0f, 0.0f, 120.0f / 255.0f}; ///< Semi-transparent black.

        void OnDraw(UIDrawContext& ctx) override
        {
            // Themeable scrim: background-color rule wins over the field default.
            ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()},
                              ResolveStyleColor(StyleProperty::Background, Color));
        }

        // Block all mouse input so nothing reaches content behind the modal.
        void OnMouseDown(MouseEventArgs& e) override { e.Handled = true; }
        void OnMouseUp(MouseEventArgs& e) override { e.Handled = true; }
        void OnMouseMove(MouseEventArgs& e) override { e.Handled = true; }
    };

    RTTI_DEFINE_OBJECT(ModalBackdrop, "rtti::ui")
}
