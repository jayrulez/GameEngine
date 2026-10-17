// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :tooltip partition
//
// Tooltip: a small text panel shown after the pointer rests on a widget for a delay. Modeled
// on eepp's tooltip behavior (role only). The Tooltip is the visual (a bordered label panel);
// a TooltipManager watches the EventDispatcher's hovered node and, once it has rested for the
// delay on a widget with tooltip text (UIWidget::SetTooltip), shows the tooltip near the
// cursor. Any hover change hides it. The manager is ticked each frame (mouse pos + hovered
// node come from the dispatcher; the app supplies the delta time).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:tooltip;

import foundation.core;  // RefPtr, MakeRef, String, StringView, Float2, Max
import foundation.fonts; // CachedFont
import :rect;
import :draw_context;
import :text;
import :rectangle_drawable;
import :node;
import :ui_widget;
import :event_dispatcher; // the popup stack (E5)
import :popup_placement;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class Tooltip : public UIWidget
    {
        RTTI_OBJECT(Tooltip, UIWidget)
    public:
        Tooltip()
        {
            SetTag(core::StringView(u8"tooltip"));
            SetHitTestVisible(false); // never steals pointer events
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetPadding(Thickness{6.0f, 3.0f, 6.0f, 3.0f});
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_bgColor));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
        }

        void SetText(core::StringView text)
        {
            m_text.SetString(text);
            Invalidate();
        }

        // Natural size for the text plus padding.
        [[nodiscard]] core::Float2 MeasureText() const
        {
            const core::Float2 t = m_text.Measure();
            const Thickness pad = GetPadding();
            return core::Float2{t.x + pad.TotalHorizontal(), t.y + pad.TotalVertical()};
        }

    protected:
        // Its text draws in its text style.
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_text.SetColor(GetTextColor());
        }
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            m_text.Draw(ctx, GetContentBounds());
        }

    private:
        Text m_text;
        Color m_bgColor{0.10f, 0.11f, 0.14f, 0.96f};
    };

    // Drives tooltip show/hide from the dispatcher's hover state. Tick it each frame.
    class TooltipManager
    {
    public:
        TooltipManager() { m_tooltip = core::MakeRef<Tooltip>(foundation::core::DefaultAllocator()); }

        void SetFont(fonts::CachedFont* font) { m_tooltip->SetFont(font); }
        void SetDelay(f64 seconds) noexcept { m_delay = seconds; }
        [[nodiscard]] Tooltip* GetTooltip() const noexcept { return m_tooltip.Get(); }
        /// Shown: a hint popup (E5), which any press or key closes.
        [[nodiscard]] bool IsShown() const
        {
            const EventDispatcher* d = m_tooltip->GetEventDispatcher();
            return d != nullptr && d->IsPopupOpen(*m_tooltip);
        }

        // Advance by `dt` seconds; `root` is where the tooltip is attached when shown.
        void Update(EventDispatcher& dispatcher, Node& root, f64 dt)
        {
            Node* over = dispatcher.GetOverNode();
            if (over != m_lastOver)
            {
                m_lastOver = over;
                m_timer = 0.0;
                m_closedHere = false;
                Hide();
            }

            const core::StringView text =
                (over != nullptr) ? over->GetTooltipText() : core::StringView{};
            if (text.Size() == 0)
            {
                Hide();
                return;
            }

            if (!IsShown() && !m_closedHere)
            {
                m_timer += dt;
                if (m_timer >= m_delay)
                    Show(dispatcher, root, root.ConvertToNodeSpace(dispatcher.GetMousePosition()), text); // device pixels to the root's units
            }
        }

        void HideNow() { Hide(); }

    private:
        // A hint popup of the root, below and right of the pointer, flipped and slid in at edges.
        void Show(EventDispatcher& dispatcher, Node& root, core::Float2 mouse, core::StringView text)
        {
            m_tooltip->SetText(text);
            m_tooltip->SetSize(m_tooltip->MeasureText());
            PopupOptions options;
            options.Kind = PopupKind::Hint;
            options.AnchorRect = Rect{mouse.x + 12.0f, mouse.y, 0.0f, 0.0f}; // right of the pointer, its gap below
            options.HasAnchorRect = true;
            options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 18.0f, false};
            TooltipManager* self = this;
            options.OnClosed = [self](PopupCloseReason reason)
            {
                if (reason != PopupCloseReason::Code)
                    self->m_closedHere = true; // a press or a key hid it: not again until the hover moves
            };
            dispatcher.OpenPopup(root, *m_tooltip, core::Move(options));
        }
        void Hide()
        {
            if (EventDispatcher* d = m_tooltip->GetEventDispatcher())
                (void)d->ClosePopup(*m_tooltip);
        }

        RefPtr<Tooltip> m_tooltip;
        Node* m_lastOver = nullptr;
        f64 m_delay = 0.5;
        f64 m_timer = 0.0;
        bool m_closedHere = false; // closed by a press or a key under the same hover
    };

    RTTI_DEFINE_OBJECT(Tooltip, "rtti::gui")
}
