// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :draw_context partition
//
// DrawContext: the renderer seam eepp's UI lacked. eepp widgets/drawables draw straight
// through global Primitives / GlobalBatchRenderer / GLi; here every draw call routes
// through a VGContext instead. Thin wrapper (mirrors foundation.ui's UIDrawContext) adding
// clip / transform / opacity stacking and the DPI scale; the font service is not wired
// in here.

module;
#include "Core/Prelude.h"

export module experimental.gui:draw_context;

import foundation.core; // Float4x4
import foundation.vg;   // VGContext
import :rect;
import :transform2d;

using namespace foundation::core;
namespace vg = foundation::vg;

export namespace experimental::gui
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

        /// Device pixels per local unit under the current transform (its area scale's root), for
        /// sizes VG takes in device pixels: a blur's sigma.
        [[nodiscard]] f32 DeviceScale() const noexcept
        {
            const Float4x4 m = m_vg->GetTransform();
            return Sqrt(Abs(m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0)));
        }

        // Concatenate a local transform onto the current one (child-local applied first).
        void ConcatTransform(const Transform2D& transform)
        {
            m_vg->SetTransform(transform.ToMatrix() * m_vg->GetTransform());
        }

        // What a draw did (E8a): the nodes it drew and the ones it culled.
        void CountDrawn() noexcept { ++m_nodesDrawn; }
        void CountCulled() noexcept { ++m_nodesCulled; }
        [[nodiscard]] u32 NodesDrawn() const noexcept { return m_nodesDrawn; }
        [[nodiscard]] u32 NodesCulled() const noexcept { return m_nodesCulled; }

    private:
        vg::VGContext* m_vg;
        f32 m_dpiScale;
        u32 m_nodesDrawn = 0;
        u32 m_nodesCulled = 0;
    };
}
