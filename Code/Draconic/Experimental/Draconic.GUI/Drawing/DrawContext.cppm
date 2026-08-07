// Draconic GUI - :draw_context partition
//
// DrawContext: the renderer seam eepp's UI lacked. eepp widgets/drawables draw straight
// through global Primitives / GlobalBatchRenderer / GLi; here every draw call routes
// through a VGContext instead. Thin wrapper (mirrors draconic.ui's UIDrawContext) adding
// clip / transform / opacity stacking and the DPI scale; the font service is added when
// the text phase lands.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.gui:draw_context;

import draconic.foundation; // Float4x4
import draconic.vg;   // VGContext
import :rect;
import :transform2d;

using namespace draconic::foundation;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    class DrawContext
    {
    public:
        explicit DrawContext(vg::VGContext& context, f32 dpiScale = 1.0f) noexcept
            : m_vg(&context), m_dpiScale(dpiScale)
        {
        }

        // The underlying vector-graphics context (the actual draw surface).
        [[nodiscard]] vg::VGContext& VG() const noexcept { return *m_vg; }
        [[nodiscard]] f32 DpiScale() const noexcept { return m_dpiScale; }

        // Clip stack (device/local rect).
        void PushClip(const Rect& rect) { m_vg->PushClipRect(rect.ToRectangle()); }
        void PopClip() { m_vg->PopClip(); }

        // Opacity stack (multiplies down the subtree).
        void PushOpacity(f32 opacity) { m_vg->PushOpacity(opacity); }
        void PopOpacity() { m_vg->PopOpacity(); }

        // Save/restore the whole VG state (transform + clip + opacity + blend).
        void Save() { m_vg->PushState(); }
        void Restore() { m_vg->PopState(); }

        // Concatenate a local transform onto the current one (child-local applied first).
        void ConcatTransform(const Transform2D& transform)
        {
            m_vg->SetTransform(transform.ToMatrix() * m_vg->GetTransform());
        }

    private:
        vg::VGContext* m_vg;
        f32 m_dpiScale;
    };
}
