// Draconic UI - :context_menu partition
//
// Popup context menu with themed items, submenus, separators, and full keyboard navigation. Shown via
// PopupLayer. Ported from Sedulous.UI/src/Overlay/ContextMenu.bf + MenuItem.bf. MenuItem + ContextMenu
// share this ONE partition because they are mutually recursive (MenuItem owns a submenu ContextMenu;
// ContextMenu owns MenuItems) - a cycle C++ module partitions cannot express as a DAG (same precedent as
// ComboBox+ComboBoxDropdown). OWNERSHIP: Beef `List<MenuItem>` of heap ptrs + manual delete -> Array<
// UniquePtr<MenuItem>> (stable element pointers for AddSubmenu's return, RAII cleanup); MenuItem.Action
// `delegate void()` -> Function<void()> (captures own their state, so Beef's mOwnedObjects is unnecessary
// and dropped); MenuItem.Submenu owned as RefPtr<ContextMenu>. Menus are shown ownsView:true so the
// PopupLayer holds the only surviving ref; submenus are shown ownsView:false (parent MenuItem owns them).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:context_menu;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import :view;
import :box_constraints;
import :draw_context;
import :drawable;
import :control_state;
import :style_property;
import :event_args;
import :input_enums;
import :palette;
import :ipopup_owner;
import :popup_layer;
import :popup_positioner;

using namespace draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::ui
{
    class ContextMenu;

    /// A single item in a ContextMenu. Owns its action delegate and optional submenu.
    class MenuItem
    {
    public:
        String Label;
        Function<void()> Action;
        bool Enabled = true;
        bool IsSeparator = false;
        // Submenu is a ContextMenu, but held as RefPtr<View> (its base): a RefPtr specialization of the
        // still-incomplete ContextMenu here, combined with importing :popup_layer, trips a gcc-15 modules
        // bug ("failed to load pendings for RefPtr" reading the :popup_layer cluster). RefPtr<View> is an
        // already-materialized specialization, so it sidesteps the bug; ContextMenu recovers it via Cast.
        RefPtr<View> Submenu;

        MenuItem() = default;
        MenuItem(StringView label, Function<void()> action, bool enabled = true)
            : Label(label), Action(Move(action)), Enabled(enabled)
        {
        }

        static UniquePtr<MenuItem> CreateSeparator()
        {
            UniquePtr<MenuItem> item = MakeUnique<MenuItem>(DefaultAllocator());
            item->IsSeparator = true;
            return item;
        }
    };

    /// Popup context menu with themed items. Supports submenus, separators, and full keyboard
    /// navigation. Shown via PopupLayer.
    class ContextMenu : public View, public IPopupOwner
    {
        DRACONIC_OBJECT(ContextMenu, View)
    public:
        ContextMenu()
        {
            IsFocusable = true;
            AddClass(u8"contextmenu");
        }

        [[nodiscard]] i32 ItemCount() const noexcept { return static_cast<i32>(m_items.Size()); }

        void AddItem(StringView label, Function<void()> action, bool enabled = true)
        {
            m_items.PushBack(
                MakeUnique<MenuItem>(DefaultAllocator(), label, Move(action), enabled));
        }

        void AddSeparator() { m_items.PushBack(MenuItem::CreateSeparator()); }

        MenuItem* AddSubmenu(StringView label)
        {
            UniquePtr<MenuItem> item = MakeUnique<MenuItem>(DefaultAllocator());
            item->Label = String(label);
            RefPtr<ContextMenu> submenu = MakeRef<ContextMenu>(DefaultAllocator());
            submenu->m_parentMenu = this;
            item->Submenu = submenu;
            MenuItem* raw = item.Get();
            m_items.PushBack(Move(item));
            return raw;
        }

        /// Index of the item currently rendered highlighted (-1 = none).
        [[nodiscard]] i32 HoveredIndex() const { return m_hoveredIndex; }

        /// Show this menu at the given screen position.
        void Show(UIContext* ctx, f32 x, f32 y, IPopupOwner* owner = nullptr)
        {
            // Menus are RETAINED views (a menu bar reuses its ContextMenu instances), so
            // the previous session's hover survives the close - reopening after "Close
            // Project" showed that item still highlighted until the mouse first moved.
            // Every show starts unhighlighted.
            m_hoveredIndex = -1;

            RootView* root = ctx->ActiveInputRoot();
            if (root == nullptr)
            {
                return;
            }

            const Float2 logical = root->LogicalSize();
            Measure(BoxConstraints::Loose(logical.x, logical.y));
            const Rectangle screen{0, 0, logical.x, logical.y};

            f32 px = x;
            f32 py = y;
            if (px + MeasuredSize.x > screen.width)
            {
                px = Max(0.0f, px - MeasuredSize.x);
            }
            if (py + MeasuredSize.y > screen.height)
            {
                py = Max(0.0f, py - MeasuredSize.y);
            }

            root->GetPopupLayer()->ShowPopup(this, owner, px, py, true, false, true);

            // Request focus for keyboard navigation.
            ctx->GetFocusManager()->SetFocus(this);
        }

        /// Close this menu and all submenus.
        void Close()
        {
            CloseOpenSubmenu();
            UIContext* ctx = Context;
            if (ctx != nullptr)
            {
                ContextMenu* self = this;
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[ctx, self]()
                                     {
                                         if (RootView* root = ctx->ActiveInputRoot())
                                         {
                                             root->GetPopupLayer()->ClosePopup(self);
                                         }
                                     }});
            }
        }

        /// Close the entire menu chain from root to leaf.
        void CloseEntireChain()
        {
            ContextMenu* root = this;
            while (root->m_parentMenu != nullptr)
            {
                root = root->m_parentMenu;
            }
            root->CloseOpenSubmenu();
            UIContext* ctx = root->Context;
            if (ctx != nullptr)
            {
                ctx->MutationQueueRef().QueueAction(
                    Function<void()>{[ctx, root]()
                                     {
                                         if (RootView* r = ctx->ActiveInputRoot())
                                         {
                                             r->GetPopupLayer()->ClosePopup(root);
                                         }
                                     }});
            }
        }

        // === IPopupOwner ===

        void OnPopupClosed(View* popup) override
        {
            if (m_openSubmenu != nullptr && popup == m_openSubmenu)
            {
                m_openSubmenu = nullptr;
            }
        }

        [[nodiscard]] View* OwnerView() override { return this; }

        // === Drawing ===

        void OnDraw(UIDrawContext& ctx) override
        {
            const f32 w = Width();
            const f32 h = Height();
            const Rectangle menuBounds{0, 0, w, h};

            // Background from theme.
            Drawable* bg = ResolveStyleDrawable(StyleProperty::Background);
            if (bg != nullptr)
            {
                bg->Draw(ctx, menuBounds, GetControlState());
            }
            else
            {
                ctx.VG().FillRoundedRect(
                    menuBounds, 4.0f, Color{45.0f / 255.0f, 48.0f / 255.0f, 58.0f / 255.0f, 1.0f});
                ctx.VG().StrokeRoundedRect(
                    menuBounds, 4.0f, Color{70.0f / 255.0f, 75.0f / 255.0f, 90.0f / 255.0f, 1.0f},
                    1.0f);
            }

            const Color textColor =
                ResolveStyleColor(StyleProperty::TextColor,
                                  Color{220.0f / 255.0f, 225.0f / 255.0f, 235.0f / 255.0f, 1.0f});
            const Color disabledColor = Palette::ComputeDisabled(textColor);
            const Color separatorColor =
                ResolveStyleColor(StyleProperty::BorderColor,
                                  Color{70.0f / 255.0f, 75.0f / 255.0f, 90.0f / 255.0f, 1.0f});
            const Color hoverColor = ResolveStyleColor(
                StyleProperty::AccentColor,
                Color{60.0f / 255.0f, 120.0f / 255.0f, 200.0f / 255.0f, 100.0f / 255.0f});

            const f32 fontSize = 14.0f;
            fonts::CachedFont* font =
                ctx.FontService() != nullptr
                    ? ctx.FontService()->GetFont(ResolveStyleFontFamily(), fontSize)
                    : nullptr;

            f32 y = 4;
            for (i32 i = 0; i < static_cast<i32>(m_items.Size()); ++i)
            {
                MenuItem* item = m_items[static_cast<usize>(i)].Get();
                if (item->IsSeparator)
                {
                    const f32 sepY = y + m_separatorHeight * 0.5f;
                    ctx.VG().DrawLine(Float2{8, sepY}, Float2{w - 8, sepY}, separatorColor, 1.0f);
                    y += m_separatorHeight;
                    continue;
                }

                // Hover highlight
                if (i == m_hoveredIndex)
                {
                    const Rectangle hoverRect{4, y, w - 8, m_itemHeight};
                    Drawable* hoverDrawable =
                        ResolveStyleDrawable(StyleProperty::MenuItemHoverDrawable);
                    if (hoverDrawable != nullptr)
                    {
                        hoverDrawable->Draw(ctx, hoverRect);
                    }
                    else
                    {
                        ctx.VG().FillRect(hoverRect, hoverColor);
                    }
                }

                // Label
                if (!item->Label.IsEmpty() && font != nullptr)
                {
                    const Color color = item->Enabled ? textColor : disabledColor;
                    ctx.VG().DrawText(item->Label, font, Rectangle{12, y, w - 24, m_itemHeight},
                                      fonts::TextAlignment::Left, fonts::VerticalAlignment::Middle,
                                      color);
                }

                // Submenu arrow
                if (item->Submenu)
                {
                    Drawable* arrowIcon = ResolvePartDrawable(
                        u8"submenu-arrow", StyleProperty::Background, GetControlState());
                    const f32 arrowX = w - 16;
                    const f32 arrowCY = y + m_itemHeight * 0.5f;
                    const f32 arrowSize = 6.0f;
                    if (arrowIcon != nullptr)
                    {
                        arrowIcon->Draw(ctx, Rectangle{arrowX, arrowCY - arrowSize * 0.5f,
                                                       arrowSize, arrowSize});
                    }
                    else
                    {
                        ctx.VG().BeginPath();
                        ctx.VG().MoveTo(arrowX, arrowCY - arrowSize * 0.5f);
                        ctx.VG().LineTo(arrowX + arrowSize * 0.6f, arrowCY);
                        ctx.VG().LineTo(arrowX, arrowCY + arrowSize * 0.5f);
                        ctx.VG().ClosePath();
                        ctx.VG().Fill(textColor);
                    }
                }

                y += m_itemHeight;
            }
        }

        // === Input ===

        void OnMouseMove(MouseEventArgs& e) override
        {
            const i32 newIndex = GetItemIndexAt(e.Y);
            if (newIndex != m_hoveredIndex)
            {
                m_hoveredIndex = newIndex;
                Invalidate();

                CloseOpenSubmenu();

                if (newIndex >= 0 && newIndex < static_cast<i32>(m_items.Size()))
                {
                    MenuItem* item = m_items[static_cast<usize>(newIndex)].Get();
                    if (item->Submenu && item->Enabled)
                    {
                        OpenSubmenuAt(newIndex);
                    }
                }
            }
        }

        void OnMouseDown(MouseEventArgs& e) override
        {
            const i32 index = GetItemIndexAt(e.Y);
            if (index >= 0 && index < static_cast<i32>(m_items.Size()))
            {
                MenuItem* item = m_items[static_cast<usize>(index)].Get();
                if (item->Enabled && !item->IsSeparator && !item->Submenu)
                {
                    if (item->Action)
                    {
                        item->Action();
                    }
                    CloseEntireChain();
                    e.Handled = true;
                }
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            switch (e.Key)
            {
            case KeyCode::Up:
                MoveFocusPrev();
                e.Handled = true;
                break;
            case KeyCode::Down:
                MoveFocusNext();
                e.Handled = true;
                break;
            case KeyCode::Right:
                // Open submenu
                if (m_hoveredIndex >= 0 && m_hoveredIndex < static_cast<i32>(m_items.Size()))
                {
                    MenuItem* item = m_items[static_cast<usize>(m_hoveredIndex)].Get();
                    if (item->Submenu && item->Enabled)
                    {
                        OpenSubmenuAt(m_hoveredIndex);
                        if (m_openSubmenu != nullptr)
                        {
                            if (Context != nullptr)
                            {
                                Context->GetFocusManager()->SetFocus(m_openSubmenu);
                            }
                            m_openSubmenu->MoveFocusNext();
                        }
                    }
                }
                e.Handled = true;
                break;
            case KeyCode::Left:
                // Close to parent
                if (m_parentMenu != nullptr)
                {
                    Close();
                }
                e.Handled = true;
                break;
            case KeyCode::Return:
                if (m_hoveredIndex >= 0 && m_hoveredIndex < static_cast<i32>(m_items.Size()))
                {
                    MenuItem* item = m_items[static_cast<usize>(m_hoveredIndex)].Get();
                    if (item->Enabled && !item->IsSeparator)
                    {
                        if (item->Submenu)
                        {
                            OpenSubmenuAt(m_hoveredIndex);
                            if (m_openSubmenu != nullptr)
                            {
                                if (Context != nullptr)
                                {
                                    Context->GetFocusManager()->SetFocus(m_openSubmenu);
                                }
                                m_openSubmenu->MoveFocusNext();
                            }
                        }
                        else
                        {
                            if (item->Action)
                            {
                                item->Action();
                            }
                            CloseEntireChain();
                        }
                    }
                }
                e.Handled = true;
                break;
            case KeyCode::Escape:
                if (m_parentMenu != nullptr)
                {
                    Close();
                }
                else
                {
                    CloseEntireChain();
                }
                e.Handled = true;
                break;
            default:
                break;
            }
        }

    protected:
        // === Measurement ===

        void OnMeasure(BoxConstraints constraints) override
        {
            f32 totalH = 4; // top padding
            f32 maxW = m_minWidth;

            for (const UniquePtr<MenuItem>& itemPtr : m_items)
            {
                MenuItem* item = itemPtr.Get();
                if (item->IsSeparator)
                {
                    totalH += m_separatorHeight;
                }
                else
                {
                    totalH += m_itemHeight;
                }

                if (!item->Label.IsEmpty() && Context != nullptr &&
                    Context->FontService() != nullptr)
                {
                    if (fonts::CachedFont* font =
                            Context->FontService()->GetFont(ResolveStyleFontFamily(), 14.0f))
                    {
                        const f32 textW = font->font->MeasureString(item->Label) + 40;
                        maxW = Max(maxW, textW);
                    }
                }
            }
            totalH += 4; // bottom padding

            MeasuredSize =
                Float2{constraints.ConstrainWidth(maxW), constraints.ConstrainHeight(totalH)};
        }

    private:
        // === Keyboard navigation helpers ===

        void MoveFocusNext()
        {
            const i32 count = static_cast<i32>(m_items.Size());
            const i32 start = m_hoveredIndex;
            for (i32 i = 1; i <= count; ++i)
            {
                const i32 idx = (start + i) % count;
                if (!m_items[static_cast<usize>(idx)]->IsSeparator)
                {
                    m_hoveredIndex = idx;
                    Invalidate();
                    return;
                }
            }
        }

        void MoveFocusPrev()
        {
            const i32 count = static_cast<i32>(m_items.Size());
            const i32 start = (m_hoveredIndex < 0) ? 0 : m_hoveredIndex;
            for (i32 i = 1; i <= count; ++i)
            {
                const i32 idx = (start - i + count) % count;
                if (!m_items[static_cast<usize>(idx)]->IsSeparator)
                {
                    m_hoveredIndex = idx;
                    Invalidate();
                    return;
                }
            }
        }

        // === Internal ===

        [[nodiscard]] i32 GetItemIndexAt(f32 localY) const
        {
            f32 y = 4;
            for (i32 i = 0; i < static_cast<i32>(m_items.Size()); ++i)
            {
                const f32 h =
                    m_items[static_cast<usize>(i)]->IsSeparator ? m_separatorHeight : m_itemHeight;
                if (localY >= y && localY < y + h)
                {
                    return m_items[static_cast<usize>(i)]->IsSeparator ? -1 : i;
                }
                y += h;
            }
            return -1;
        }

        [[nodiscard]] f32 GetItemY(i32 index) const
        {
            f32 y = 4;
            for (i32 i = 0; i < index; ++i)
            {
                y += m_items[static_cast<usize>(i)]->IsSeparator ? m_separatorHeight : m_itemHeight;
            }
            return y;
        }

        void OpenSubmenuAt(i32 index)
        {
            MenuItem* item = m_items[static_cast<usize>(index)].Get();
            if (!item->Submenu || Context == nullptr)
            {
                return;
            }
            ContextMenu* submenu = Cast<ContextMenu>(item->Submenu.Get());
            if (submenu == nullptr)
            {
                return;
            }

            RootView* root = Context->ActiveInputRoot();
            if (root == nullptr)
            {
                return;
            }

            const Float2 logical = root->LogicalSize();
            const Float2 pos = PopupPositioner::Submenu(
                Rectangle{Bounds.x, Bounds.y + GetItemY(index), Width(), m_itemHeight},
                Float2{submenu->m_minWidth, 200}, Rectangle{0, 0, logical.x, logical.y});

            m_openSubmenu = submenu;
            m_submenuLayer = root->GetPopupLayer();
            root->GetPopupLayer()->ShowPopup(submenu, this, pos.x, pos.y, false, false, false);
        }

        void CloseOpenSubmenu()
        {
            if (m_openSubmenu != nullptr)
            {
                m_openSubmenu->CloseOpenSubmenu(); // recursive
                if (m_submenuLayer != nullptr)
                {
                    m_submenuLayer->ClosePopup(m_openSubmenu);
                }
                m_openSubmenu = nullptr;
            }
        }

    public:
        ~ContextMenu() override
        {
            // Close any open submenu from PopupLayer before items are deleted.
            if (m_openSubmenu != nullptr && m_submenuLayer != nullptr)
            {
                m_submenuLayer->ClosePopup(m_openSubmenu);
                m_openSubmenu = nullptr;
            }
        }

    private:
        Array<UniquePtr<MenuItem>> m_items;
        i32 m_hoveredIndex = -1;
        ContextMenu* m_openSubmenu = nullptr;
        ContextMenu* m_parentMenu = nullptr;
        PopupLayer* m_submenuLayer = nullptr; // non-owning ref for cleanup
        f32 m_itemHeight = 28;
        f32 m_separatorHeight = 8;
        f32 m_minWidth = 150;
    };

    DRACONIC_DEFINE_OBJECT(ContextMenu, "draconic::ui")
}
