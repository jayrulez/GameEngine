// Draconic UI Toolkit - :menu_bar partition
//
// Horizontal menu bar with dropdown ContextMenus. Ported from Sedulous.UI.Toolkit/src/MenuBar.bf.
// Beef `List<MenuEntry{String Title; ContextMenu Menu}>` with manual delete -> Array<MenuEntry> holding
// String + RefPtr<ContextMenu> (the bar owns the menus; PopupLayer shows them ownsView:false). The bar
// implements IPopupOwner so PopupLayer routes close-notifications back. `Root.PopupLayer` -> Root()->
// GetPopupLayer(); `Context.InputManager`/measurement via Root()->ViewportSize.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui.toolkit:menu_bar;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import draconic.ui;

using namespace draconic::foundation;

export namespace draconic::ui::toolkit
{
    /// Horizontal menu bar with dropdown ContextMenus. Click a title to open its dropdown; while open,
    /// hover other titles to switch. Escape or click-outside closes.
    class MenuBar : public ViewGroup, public IPopupOwner
    {
        DRACONIC_OBJECT(MenuBar, ViewGroup)
    public:
        MenuBar() { IsFocusable = true; }

        [[nodiscard]] usize MenuCount() const noexcept { return m_menus.Size(); }

        /// Add a menu with the given title. Returns the borrowed ContextMenu to add items to.
        ContextMenu* AddMenu(StringView title)
        {
            MenuEntry entry;
            entry.Title = String(title);
            entry.Menu = MakeRef<ContextMenu>(DefaultAllocator());
            ContextMenu* raw = entry.Menu.Get();
            m_menus.PushBack(Move(entry));
            Invalidate();
            return raw;
        }

        // === IPopupOwner ===

        void OnPopupClosed(View* popup) override
        {
            (void)popup;
            m_activeIndex = -1;
            m_menuMode = false;
        }

        [[nodiscard]] View* OwnerView() override { return this; }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 w = Width();
            const f32 h = Height();

            // Background.
            if (Drawable* bgDrawable = ResolveStyleDrawable(StyleProperty::Background))
            {
                bgDrawable->Draw(ctx, Rectangle{0, 0, w, h});
            }
            else
            {
                ctx.VG().FillRect(Rectangle{0, 0, w, h}, Rgb(35, 37, 46, 255));
            }

            // Bottom border.
            const Color borderColor =
                ResolveStyleColor(StyleProperty::BorderColor, Rgb(65, 70, 85, 255));
            ctx.VG().FillRect(Rectangle{0, h - 1.0f, w, 1.0f}, borderColor);

            // Rebuild item rects.
            RebuildItemRects(ctx);

            const Color hoverColor =
                ResolveStyleColor(StyleProperty::AccentColor, Rgb(60, 65, 80, 255));
            const Color textColor =
                ResolveStyleColor(StyleProperty::TextColor, Rgb(220, 225, 235, 255));

            if (ctx.FontService() != nullptr)
            {
                fonts::CachedFont* font = ctx.FontService()->GetFont(m_fontSize);
                if (font != nullptr)
                {
                    for (usize i = 0; i < m_menus.Size() && i < m_itemRects.Size(); ++i)
                    {
                        const Rectangle rect = m_itemRects[i];

                        // Hover/active highlight (rounded in the rounded theme).
                        if (static_cast<i32>(i) == m_activeIndex ||
                            static_cast<i32>(i) == m_hoveredIndex)
                        {
                            const f32 cr = ResolveStyleFloat(StyleProperty::CornerRadius, 0.0f);
                            if (cr > 0.0f)
                            {
                                ctx.VG().FillRoundedRect(rect, cr, hoverColor);
                            }
                            else
                            {
                                ctx.VG().FillRect(rect, hoverColor);
                            }
                        }

                        // Text.
                        ctx.VG().DrawText(m_menus[i].Title, font, rect,
                                          fonts::TextAlignment::Center,
                                          fonts::VerticalAlignment::Middle, textColor);
                    }
                }
            }
        }

        // === Input ===

        void OnMouseDown(MouseEventArgs& e) override
        {
            if (!IsEffectivelyEnabled() || e.Button != MouseButton::Left)
            {
                return;
            }

            const i32 clickedIdx = GetItemIndexAt(e.X, e.Y);
            if (clickedIdx < 0)
            {
                return;
            }

            if (m_activeIndex == clickedIdx && m_menuMode)
            {
                // Clicking the active menu title closes it.
                CloseActiveMenu();
            }
            else
            {
                OpenMenu(clickedIdx);
            }
            e.Handled = true;
        }

        void OnMouseMove(MouseEventArgs& e) override
        {
            const i32 idx = GetItemIndexAt(e.X, e.Y);
            m_hoveredIndex = idx;

            // Menu mode: hover-switch to a different menu.
            if (m_menuMode && idx >= 0 && idx != m_activeIndex)
            {
                OpenMenu(idx);
            }
        }

        void OnMouseLeave() override { m_hoveredIndex = -1; }

        void OnKeyDown(KeyEventArgs& e) override
        {
            if (!m_menuMode)
            {
                return;
            }

            switch (e.Key)
            {
            case KeyCode::Left:
                if (m_activeIndex > 0)
                {
                    OpenMenu(m_activeIndex - 1);
                }
                e.Handled = true;
                break;
            case KeyCode::Right:
                if (m_activeIndex < static_cast<i32>(m_menus.Size()) - 1)
                {
                    OpenMenu(m_activeIndex + 1);
                }
                e.Handled = true;
                break;
            case KeyCode::Escape:
                CloseActiveMenu();
                e.Handled = true;
                break;
            default:
                break;
            }
        }

    protected:
        void OnMeasure(BoxConstraints constraints) override
        {
            MeasuredSize =
                Float2{constraints.ConstrainWidth(0.0f), constraints.ConstrainHeight(m_itemHeight)};
        }

    private:
        struct MenuEntry
        {
            String Title;
            RefPtr<ContextMenu> Menu;
        };

        [[nodiscard]] static Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
        }

        void RebuildItemRects(UIDrawContext& ctx)
        {
            m_itemRects.Clear();
            f32 x = 0.0f;

            if (ctx.FontService() != nullptr)
            {
                fonts::CachedFont* font = ctx.FontService()->GetFont(m_fontSize);
                if (font != nullptr)
                {
                    for (const MenuEntry& entry : m_menus)
                    {
                        const f32 textW = font->font->MeasureString(entry.Title);
                        const f32 itemW = textW + m_itemPadding * 2.0f;
                        m_itemRects.PushBack(Rectangle{x, 0, itemW, m_itemHeight});
                        x += itemW;
                    }
                }
            }
        }

        void OpenMenu(i32 index)
        {
            if (index < 0 || index >= static_cast<i32>(m_menus.Size()))
            {
                return;
            }

            // Close current menu if different.
            if (m_activeIndex >= 0 && m_activeIndex != index)
            {
                CloseActiveMenu();
            }

            m_activeIndex = index;
            m_menuMode = true;

            if (Context == nullptr)
            {
                return;
            }

            MenuEntry& entry = m_menus[static_cast<usize>(index)];
            const Rectangle rect = (index < static_cast<i32>(m_itemRects.Size()))
                                       ? m_itemRects[static_cast<usize>(index)]
                                       : Rectangle{0, 0, 0, 0};

            // Screen position just below this menu item (+2px so the dropdown clears the bar's
            // bottom border instead of sitting on it).
            const Float2 screenPos = LocalToScreen(Float2{rect.x, m_itemHeight + 2.0f});

            RootView* root = Root();
            if (root == nullptr)
            {
                return;
            }

            // Show via PopupLayer directly - MenuBar owns the ContextMenu (ownsView:false).
            entry.Menu->Measure(BoxConstraints::Loose(root->ViewportSize.x, root->ViewportSize.y));
            root->GetPopupLayer()->ShowPopup(entry.Menu.Get(), this, screenPos.x, screenPos.y,
                                             /*closeOnClickOutside*/ true, /*isModal*/ false,
                                             /*ownsView*/ false);
        }

        void CloseActiveMenu()
        {
            if (m_activeIndex >= 0 && m_activeIndex < static_cast<i32>(m_menus.Size()) &&
                Context != nullptr)
            {
                ContextMenu* menu = m_menus[static_cast<usize>(m_activeIndex)].Menu.Get();
                // The menu is shown via PopupLayer - closing it triggers OnPopupClosed.
                if (menu->Context != nullptr)
                {
                    if (RootView* root = Root())
                    {
                        root->GetPopupLayer()->ClosePopup(menu);
                    }
                }
            }
            m_activeIndex = -1;
            m_menuMode = false;
        }

        [[nodiscard]] i32 GetItemIndexAt(f32 x, f32 y) const
        {
            for (usize i = 0; i < m_itemRects.Size(); ++i)
            {
                const Rectangle r = m_itemRects[i];
                if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }

        Array<MenuEntry> m_menus;
        Array<Rectangle> m_itemRects; // rebuilt each draw
        i32 m_activeIndex = -1;
        i32 m_hoveredIndex = -1;
        bool m_menuMode = false; // true when a dropdown is open
        f32 m_itemHeight = 28.0f;
        f32 m_itemPadding = 12.0f;
        f32 m_fontSize = 13.0f;
    };

    DRACONIC_DEFINE_OBJECT(MenuBar, "draconic::ui::toolkit")
}
