// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - module implementation unit: the debug overlay's input half (E8c of
// Specs/gui-lab-evolution.md), drawn by a root over its tree in its own space: the focus path,
// the tab order, the node under the pointer. Hairlines are a device pixel; text is in the host's
// debug font, and left out without one.

module;
#include "Core/Prelude.h"

module experimental.gui;

namespace core = foundation::core;
namespace vg = foundation::vg;

namespace experimental::gui
{
    namespace
    {
        constexpr core::Color kFocus{0.2f, 0.85f, 1.0f, 1.0f};
        constexpr core::Color kTab{1.0f, 0.85f, 0.2f, 1.0f};
        constexpr core::Color kPointer{1.0f, 0.3f, 0.85f, 1.0f};
        constexpr core::Color kLabelBack{0.0f, 0.0f, 0.0f, 0.75f};

        // A name tag over the top-left of `box` (inside it when the box is at the top).
        void Tag(vg::VGContext& g, fonts::CachedFont* font, const Rect& box, core::StringView text, core::Color color)
        {
            if (font == nullptr || text.Size() == 0)
                return;
            const f32 height = 16.0f;
            const f32 width = 7.0f * static_cast<f32>(text.Size()) + 8.0f; // a debug font's rough advance
            const f32 y = box.y >= height ? box.y - height : box.y;
            g.FillRect(core::Rectangle{box.x, y, width, height}, kLabelBack);
            g.DrawText(text, font, core::Rectangle{box.x + 4.0f, y, width - 4.0f, height}, fonts::TextAlignment::Left,
                       fonts::VerticalAlignment::Middle, color);
        }
    }

    void SceneRoot::DrawInputOverlay(DrawContext& ctx) const
    {
        vg::VGContext& g = ctx.VG();
        const f32 hair = 1.0f / core::Max(ctx.DeviceScale(), 0.01f);

        if (m_debugOverlay.TabOrder)
        {
            // Each stop numbered at its top-left, joined centre to centre in the order Tab takes.
            const core::Array<Node*> stops = m_eventDispatcher.TabStops();
            core::Float2 previous{};
            for (usize i = 0; i < stops.Size(); ++i)
            {
                const Rect box = InRootSpace(*stops[i]);
                const core::Float2 centre{box.x + box.width * 0.5f, box.y + box.height * 0.5f};
                if (i > 0)
                    g.DrawLine(previous, centre, core::Color{kTab.r, kTab.g, kTab.b, 0.6f}, 1.5f * hair);
                previous = centre;
                g.StrokeRect(box.ToRectangle(), core::Color{kTab.r, kTab.g, kTab.b, 0.5f}, hair);
                g.FillCircle(core::Float2{box.x, box.y}, 9.0f, kTab);
                if (m_debugFont != nullptr)
                {
                    const core::String number = core::Format(u8"{}", i + 1);
                    g.DrawText(number.AsView(), m_debugFont, core::Rectangle{box.x - 9.0f, box.y - 9.0f, 18.0f, 18.0f},
                               fonts::TextAlignment::Center, fonts::VerticalAlignment::Middle, core::Color{0.0f, 0.0f, 0.0f, 1.0f});
                }
            }
        }

        if (m_debugOverlay.FocusPath)
            if (const Node* focused = m_eventDispatcher.GetFocusNode())
            {
                // The focused node strong, each node holding it fainter the further out, and the
                // path named from the focused node outward.
                core::String path;
                f32 alpha = 0.7f;
                int depth = 0;
                for (const Node* n = focused->GetParent(); n != nullptr && n != this; n = n->GetParent())
                {
                    g.StrokeRect(InRootSpace(*n).ToRectangle(), core::Color{kFocus.r, kFocus.g, kFocus.b, alpha}, hair);
                    alpha = core::Max(0.15f, alpha * 0.75f);
                    if (depth++ < 4)
                    {
                        path += core::StringView(u8" < ");
                        path += n->DebugName().AsView();
                    }
                }
                const Rect box = InRootSpace(*focused);
                g.StrokeRect(box.ToRectangle(), kFocus, 2.0f * hair);
                core::String name = focused->DebugName();
                name += path.AsView();
                Tag(g, m_debugFont, box, name.AsView(), kFocus);
            }

        if (m_debugOverlay.Pointer)
            if (const Node* over = m_eventDispatcher.GetOverNode(); over != nullptr && over != this)
            {
                const Rect box = InRootSpace(*over);
                g.StrokeRect(box.ToRectangle(), kPointer, 2.0f * hair);
                const core::String name = core::Format(u8"{} {}x{}", over->DebugName(), static_cast<i32>(over->GetSize().x),
                                                       static_cast<i32>(over->GetSize().y));
                Tag(g, m_debugFont, Rect{box.x, box.Bottom() + 16.0f, box.width, box.height}, name.AsView(), kPointer);
            }
    }
}
