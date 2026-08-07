// Draconic UI - :modal_backdrop partition
//
// Semi-transparent backdrop drawn behind modal popups; blocks input to underlying content by consuming
// all mouse events. Ported from Sedulous.UI/src/Overlay/ModalBackdrop.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:modal_backdrop;

import draconic.foundation;
import :view;
import :draw_context;
import :event_args;

using namespace draconic::foundation;

export namespace draconic::ui
{
    // Alias so the faithful `Color` field name can still name the core Color type (field shadows type).
    using ColorValue = draconic::foundation::Color;

    class ModalBackdrop : public View
    {
        DRACONIC_OBJECT(ModalBackdrop, View)
    public:
        ColorValue Color{0.0f, 0.0f, 0.0f, 120.0f / 255.0f}; ///< Semi-transparent black.

        void OnDraw(UIDrawContext& ctx) override
        {
            ctx.VG().FillRect(Rectangle{0, 0, Width(), Height()}, Color);
        }

        // Block all mouse input so nothing reaches content behind the modal.
        void OnMouseDown(MouseEventArgs& e) override { e.Handled = true; }
        void OnMouseUp(MouseEventArgs& e) override { e.Handled = true; }
        void OnMouseMove(MouseEventArgs& e) override { e.Handled = true; }
    };

    DRACONIC_DEFINE_OBJECT(ModalBackdrop, "draconic::ui")
}
