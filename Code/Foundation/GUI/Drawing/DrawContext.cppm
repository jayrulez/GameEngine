// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :draw_context partition
//
// DrawContext: what widgets and drawables draw through. Every draw call routes through a
// VGContext rather than a global renderer. A thin wrapper adding clip, transform and opacity
// stacking and the DPI scale; the font service is not wired in here.

module;
#include "Core/Prelude.h"

export module foundation.gui:draw_context;

import foundation.core;
import foundation.vg;
import :rect;
import :transform2d;
import :picture_baker; // IPictureBaker

using namespace foundation::core;
namespace vg = foundation::vg;

export namespace foundation::gui
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
        /// Sets the host's picture baker (a root's DrawFrame sets its GUI's baker). With a baker,
        /// vector pictures are drawn as baked textures; with null, they are drawn as paths.
        void SetPictureBaker(IPictureBaker* baker) noexcept { m_baker = baker; }
        [[nodiscard]] IPictureBaker* PictureBaker() const noexcept { return m_baker; }

        // Clip stack (device/local rect).
        void PushClip(const Rect& rect) { m_vg->PushClipRect(rect.ToRectangle()); }
        void PopClip() { m_vg->PopClip(); }

        // Opacity stack (multiplies down the subtree).
        void PushOpacity(f32 opacity) { m_vg->PushOpacity(opacity); }
        void PopOpacity() { m_vg->PopOpacity(); }

        // Save and restore the whole VG state (transform, clip, opacity and blend).
        void Save() { m_vg->PushState(); }
        void Restore() { m_vg->PopState(); }

        /// Returns device pixels per local unit under the current transform (the square root of its
        /// area scale), for sizes VG takes in device pixels, such as a blur's sigma.
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

        /// Whether a rect in the current space can draw anything inside the clip, allowing a margin
        /// for what no ink box measures (text effects an ancestor sets, glyphs past their advance,
        /// anti-aliasing). Always true with culling off.
        [[nodiscard]] bool IsVisible(const Rect& local) const
        {
            if (!m_culling)
                return true;
            constexpr f32 kMargin = 32.0f;
            return m_vg->IsRectVisible(Rectangle{local.x - kMargin, local.y - kMargin, local.width + 2.0f * kMargin,
                                                 local.height + 2.0f * kMargin});
        }
        /// Gets the part of the current local space that can show: the clip mapped back into local
        /// space and grown by IsVisible's margin. Returns false when everything can show (no clip,
        /// or culling off), in which case a node with many children draws them all.
        [[nodiscard]] bool GetVisibleBounds(Rect& out) const
        {
            Rectangle clip{};
            if (!m_culling || !m_vg->GetLocalClipBounds(clip))
                return false;
            constexpr f32 kMargin = 32.0f; // the same margin IsVisible uses
            out = Rect{clip.x - kMargin, clip.y - kMargin, clip.width + 2.0f * kMargin, clip.height + 2.0f * kMargin};
            return true;
        }
        /// Turns culling on or off. With culling off every node draws whatever the clip, which the
        /// culling test uses as its reference.
        void SetCulling(bool culling) noexcept { m_culling = culling; }
        [[nodiscard]] bool IsCulling() const noexcept { return m_culling; }
        /// Sets the number of the host frame being drawn (SceneRoot::DrawFrame numbers them; 0 for a
        /// draw outside a frame). Each node drawn records it, so an animation can tell whether its
        /// node was shown.
        void SetFrame(u32 frame) noexcept { m_frame = frame; }
        [[nodiscard]] u32 Frame() const noexcept { return m_frame; }
        /// Sets where each drawn node that invalidated itself since its last draw records its world
        /// bounds, for the debug overlay's redraw flash. None by default.
        void SetRedrawRecorder(Array<Rect>* recorder) noexcept { m_redraws = recorder; }
        [[nodiscard]] bool RecordsRedraws() const noexcept { return m_redraws != nullptr; }
        void RecordRedraw(const Rect& world)
        {
            if (m_redraws != nullptr)
                m_redraws->PushBack(world);
        }
        /// Sets where the frame records what it changed (damage, in device pixels). None by default.
        void SetDamageRecorder(Array<Rect>* recorder) noexcept { m_damage = recorder; }
        void RecordDamage(const Rect& device)
        {
            if (m_damage != nullptr)
                m_damage->PushBack(device);
        }
        [[nodiscard]] bool RecordsDamage() const noexcept { return m_damage != nullptr; }
        /// Returns `local` in device pixels under the current transform: the bounds of its
        /// transformed corners.
        [[nodiscard]] Rect DeviceRect(const Rect& local) const
        {
            const core::Float4x4 m = m_vg->GetTransform();
            const core::Float2 corners[4] = {core::TransformPoint2D(core::Float2{local.x, local.y}, m),
                                             core::TransformPoint2D(core::Float2{local.Right(), local.y}, m),
                                             core::TransformPoint2D(core::Float2{local.x, local.Bottom()}, m),
                                             core::TransformPoint2D(core::Float2{local.Right(), local.Bottom()}, m)};
            core::Float2 mn = corners[0], mx = corners[0];
            for (const core::Float2& c : corners)
            {
                mn = core::Float2{core::Min(mn.x, c.x), core::Min(mn.y, c.y)};
                mx = core::Float2{core::Max(mx.x, c.x), core::Max(mx.y, c.y)};
            }
            return Rect{mn.x, mn.y, mx.x - mn.x, mx.y - mn.y};
        }
        void CountDrawn() noexcept { ++m_nodesDrawn; }
        void CountCulled() noexcept { ++m_nodesCulled; }
        [[nodiscard]] u32 NodesDrawn() const noexcept { return m_nodesDrawn; }
        [[nodiscard]] u32 NodesCulled() const noexcept { return m_nodesCulled; }

    private:
        vg::VGContext* m_vg;
        f32 m_dpiScale;
        IPictureBaker* m_baker = nullptr;
        u32 m_nodesDrawn = 0;
        u32 m_nodesCulled = 0;
        bool m_culling = true;
        u32 m_frame = 0;
        Array<Rect>* m_redraws = nullptr;
        Array<Rect>* m_damage = nullptr; // what the frame changed
    };
}
