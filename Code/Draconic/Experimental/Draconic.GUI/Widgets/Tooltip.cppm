// Draconic GUI - :tooltip partition
//
// Tooltip: a small text panel shown after the pointer rests on a widget for a delay. Modeled
// on eepp's tooltip behavior (role only). The Tooltip is the visual (a bordered label panel);
// a TooltipManager watches the EventDispatcher's hovered node and, once it has rested for the
// delay on a widget with tooltip text (UIWidget::SetTooltip), shows the tooltip near the
// cursor. Any hover change hides it. The manager is ticked each frame (mouse pos + hovered
// node come from the dispatcher; the app supplies the delta time).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:tooltip;

import draconic.foundation;  // RefPtr, MakeRef, String, StringView, Float2, Max
import draconic.fonts; // CachedFont
import :rect;
import :draw_context;
import :text;
import :rectangle_drawable;
import :node;
import :ui_widget;
import :event_dispatcher;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    class Tooltip : public UIWidget
    {
        DRACONIC_OBJECT(Tooltip, UIWidget)
    public:
        Tooltip()
        {
            SetTag(foundation::StringView(u8"tooltip"));
            SetHitTestVisible(false); // never steals pointer events
            SetPadding(Thickness{6.0f, 3.0f, 6.0f, 3.0f});
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_bgColor));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
        }

        void SetText(foundation::StringView text)
        {
            m_text.SetString(text);
            Invalidate();
        }
        void SetFont(fonts::CachedFont* font)
        {
            m_text.SetFont(font);
            Invalidate();
        }
        void SetTextColor(Color color)
        {
            m_text.SetColor(color);
            Invalidate();
        }

        // Natural size for the text plus padding.
        [[nodiscard]] foundation::Float2 MeasureContent() const
        {
            const foundation::Float2 t = m_text.Measure();
            const Thickness pad = GetPadding();
            return foundation::Float2{t.x + pad.TotalHorizontal(), t.y + pad.TotalVertical()};
        }

    protected:
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
        TooltipManager() { m_tooltip = foundation::MakeRef<Tooltip>(foundation::DefaultAllocator()); }

        void SetFont(fonts::CachedFont* font) { m_tooltip->SetFont(font); }
        void SetDelay(f64 seconds) noexcept { m_delay = seconds; }
        [[nodiscard]] Tooltip* GetTooltip() const noexcept { return m_tooltip.Get(); }
        [[nodiscard]] bool IsShown() const noexcept { return m_shown; }

        // Advance by `dt` seconds; `root` is where the tooltip is attached when shown.
        void Update(EventDispatcher& dispatcher, Node& root, f64 dt)
        {
            Node* over = dispatcher.GetOverNode();
            if (over != m_lastOver)
            {
                m_lastOver = over;
                m_timer = 0.0;
                Hide();
            }

            const foundation::StringView text =
                (over != nullptr) ? over->GetTooltipText() : foundation::StringView{};
            if (text.Size() == 0)
            {
                Hide();
                return;
            }

            if (!m_shown)
            {
                m_timer += dt;
                if (m_timer >= m_delay)
                    Show(root, dispatcher.GetMousePosition(), text);
            }
        }

        void HideNow() { Hide(); }

    private:
        void Show(Node& root, foundation::Float2 mouse, foundation::StringView text)
        {
            m_tooltip->SetText(text);
            const foundation::Float2 size = m_tooltip->MeasureContent();
            m_tooltip->SetSize(size);
            m_tooltip->SetPosition(foundation::Float2{mouse.x + 12.0f, mouse.y + 18.0f});
            root.AddChild(m_tooltip.Get());
            m_shown = true;
        }
        void Hide()
        {
            if (!m_shown)
                return;
            m_tooltip->RemoveFromParent();
            m_shown = false;
        }

        RefPtr<Tooltip> m_tooltip;
        Node* m_lastOver = nullptr;
        f64 m_delay = 0.5;
        f64 m_timer = 0.0;
        bool m_shown = false;
    };

    DRACONIC_DEFINE_OBJECT(Tooltip, "draconic::gui")
}
