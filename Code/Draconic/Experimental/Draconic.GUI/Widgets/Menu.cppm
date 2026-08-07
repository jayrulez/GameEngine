// Draconic GUI - :menu partition
//
// Menu (PopupMenu): a floating vertical list of rows - a context menu or a menu dropped from a
// menu bar. Modeled on eepp's UIMenu/UIPopUpMenu family (role only). Open() shows it as a
// top-level popup via the EventDispatcher's popup support, so an outside click or Escape
// dismisses it. Rows come in a small hierarchy mirroring eepp:
//   MenuRow          - base row (height + hover-notify hook)
//     MenuSeparator  - a non-interactive divider line
//     MenuItem       - an activatable row: text + optional check mark / icon / shortcut column
//       MenuSubItem  - an item that opens a nested submenu on hover or click
//
// Submenus follow eepp's model: the submenu is a SIBLING in the tree (added to the root, not
// nested inside the parent menu), positioned to the right of its item, and the parent tracks a
// single "current submenu". Dismissal spans the whole open chain via the dispatcher's popup
// `contains` predicate (the analog of eepp's isChildOrSubMenu): the root menu claims every node
// in its submenu chain as "inside", so a click within any open submenu does not dismiss.
// Activating a leaf item closes the entire chain back to the root.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:menu;

import draconic.foundation;  // RefPtr, MakeRef, Array, Function, Move, Max, Cast
import draconic.fonts; // CachedFont
import draconic.vg;    // PathBuilder, StrokeStyle
import :rect;
import :event;
import :draw_context;
import :drawable;
import :rectangle_drawable;
import :text;
import :node;
import :ui_widget;
import :event_dispatcher;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;
namespace vg = draconic::vg;

export namespace draconic::gui
{
    // === Row base ===============================================================
    // A row in a menu. Reports its preferred height and, on hover, notifies the owning menu
    // (which uses it to open/close submenus). Selectable rows are the ones keyboard navigation
    // and activation consider.
    class MenuRow : public UIWidget
    {
        DRACONIC_OBJECT(MenuRow, UIWidget)
    public:
        [[nodiscard]] virtual f32 RowHeight() const = 0;
        [[nodiscard]] virtual bool IsSelectable() const { return false; }

        // Called (with this row) whenever the pointer enters it - the menu wires this.
        void SetOnHovered(foundation::Function<void()> callback) { m_onHovered = foundation::Move(callback); }

    protected:
        void OnMouseEnter(const MouseEvent& event) override
        {
            UINode::OnMouseEnter(event);
            if (m_onHovered)
                m_onHovered();
        }

    private:
        foundation::Function<void()> m_onHovered;
    };

    // === Separator =============================================================
    class MenuSeparator : public MenuRow
    {
        DRACONIC_OBJECT(MenuSeparator, MenuRow)
    public:
        MenuSeparator() { SetTag(foundation::StringView(u8"menuseparator")); }

        [[nodiscard]] f32 RowHeight() const override { return m_height; }
        void SetLineColor(Color color)
        {
            m_line = color;
            Invalidate();
        }

        // Theming part: menuseparator::line.
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"line"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"line"))
                m_line = color;
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetLocalBounds();
            const f32 y = b.y + b.height * 0.5f;
            ctx.VG().FillRect(
                foundation::Rectangle{b.x + 8.0f, y, foundation::Max(0.0f, b.width - 16.0f), 1.0f}, m_line);
        }

    private:
        f32 m_height = 9.0f;
        Color m_line{0.32f, 0.34f, 0.40f, 1.0f};
    };

    // === Activatable item ======================================================
    class MenuItem : public MenuRow
    {
        DRACONIC_OBJECT(MenuItem, MenuRow)
    public:
        MenuItem()
        {
            SetTag(foundation::StringView(u8"menuitem"));
            m_text.SetAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_shortcut.SetAlignment(TextHAlign::Right, TextVAlign::Middle);
        }

        // Content.
        void SetText(foundation::StringView text)
        {
            m_text.SetString(text);
            Invalidate();
        }
        [[nodiscard]] foundation::StringView GetText() const { return m_text.GetString(); }
        void SetShortcut(foundation::StringView text)
        {
            m_shortcut.SetString(text);
            Invalidate();
        }
        [[nodiscard]] foundation::StringView GetShortcut() const { return m_shortcut.GetString(); }
        void SetIcon(foundation::RefPtr<Drawable> icon)
        {
            m_icon = foundation::Move(icon);
            Invalidate();
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_text.SetFont(font);
            m_shortcut.SetFont(font);
            Invalidate();
        }
        void SetTextColor(Color color)
        {
            m_text.SetColor(color);
            m_shortcut.SetColor(color);
            Invalidate();
        }
        void SetHighlightColor(Color color) { m_highlight = color; }
        void SetRowHeight(f32 height) { m_rowHeight = foundation::Max(1.0f, height); }

        // Checkable state (a leading check mark). SetChecked fires the toggle callback.
        void SetCheckable(bool checkable)
        {
            m_checkable = checkable;
            Invalidate();
        }
        [[nodiscard]] bool IsCheckable() const noexcept { return m_checkable; }
        void SetChecked(bool checked)
        {
            if (m_checked == checked)
                return;
            m_checked = checked;
            Invalidate();
            if (m_onToggled)
                m_onToggled(m_checked);
        }
        [[nodiscard]] bool IsChecked() const noexcept { return m_checked; }
        void SetCheckedSilently(bool checked)
        {
            m_checked = checked;
            Invalidate();
        } // no callback (init)
        void SetOnToggled(foundation::Function<void(bool)> callback)
        {
            m_onToggled = foundation::Move(callback);
        }

        // The user's action (run before the menu closes).
        void SetOnPicked(foundation::Function<void()> callback) { m_onPicked = foundation::Move(callback); }
        // The menu installs this to close the whole chain after activation.
        void SetOnActivated(foundation::Function<void()> callback)
        {
            m_onActivated = foundation::Move(callback);
        }

        [[nodiscard]] f32 RowHeight() const override { return m_rowHeight; }
        [[nodiscard]] bool IsSelectable() const override { return true; }

        // Preferred width for auto-sizing: gutter + text + shortcut column + right pad.
        [[nodiscard]] f32 PreferredWidth() const
        {
            const f32 textW = m_text.Measure().x;
            const f32 shortcutW =
                m_shortcut.GetString().Size() > 0 ? m_shortcut.Measure().x + 24.0f : 0.0f;
            return kGutter + textW + shortcutW + kRightPad;
        }

        // Theming: menuitem::highlight (hover row background); text via the hook below.
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"highlight"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"highlight"))
                m_highlight = color;
        }
        void SetThemeTextColor(Color color) override { SetTextColor(color); }
        void SetThemeFont(fonts::CachedFont* font) override { SetFont(font); }

    protected:
        void OnMouseClick(const MouseEvent&) override { Activate(); }

        // Run the item: toggle (if checkable), fire the user's action, then close the chain.
        virtual void Activate()
        {
            if (m_checkable)
                SetChecked(!m_checked);
            if (m_onPicked)
                m_onPicked();
            if (m_onActivated)
                m_onActivated();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetLocalBounds();
            if (IsHovered())
                ctx.VG().FillRect(b.ToRectangle(), m_highlight);

            // Left gutter: a check mark (if checked) else an icon (if set).
            if (m_checkable && m_checked)
            {
                DrawCheck(ctx, b);
            }
            else if (m_icon)
            {
                const f32 s = foundation::Max(0.0f, foundation::Max(0.0f, kGutter - 8.0f));
                m_icon->Draw(ctx, Rect{b.x + 4.0f, b.y + (b.height - s) * 0.5f, s, s});
            }

            // Text + shortcut share the inner rect (opposite alignments).
            const Rect inner{b.x + kGutter, b.y, foundation::Max(0.0f, b.width - kGutter - kRightPad),
                             b.height};
            m_text.Draw(ctx, inner);
            if (m_shortcut.GetString().Size() > 0)
                m_shortcut.Draw(ctx, inner);
        }

        // Draw a check mark stroked inside the left gutter.
        void DrawCheck(DrawContext& ctx, const Rect& b) const
        {
            const f32 cx = b.x + kGutter * 0.5f;
            const f32 cy = b.y + b.height * 0.5f;
            vg::PathBuilder pb;
            pb.MoveTo(cx - 4.0f, cy);
            pb.LineTo(cx - 1.0f, cy + 3.5f);
            pb.LineTo(cx + 5.0f, cy - 4.0f);
            ctx.VG().StrokePath(pb.ToPath(), m_checkColor, vg::StrokeStyle(1.6f));
        }

        static constexpr f32 kGutter = 22.0f;   // left column (check / icon)
        static constexpr f32 kRightPad = 14.0f; // right column (shortcut end / submenu arrow)

    private:
        Text m_text;
        Text m_shortcut;
        foundation::RefPtr<Drawable> m_icon;
        bool m_checkable = false;
        bool m_checked = false;
        f32 m_rowHeight = 26.0f;
        Color m_highlight{0.24f, 0.40f, 0.62f, 1.0f};
        Color m_checkColor{0.88f, 0.90f, 0.94f, 1.0f};
        foundation::Function<void()> m_onPicked;
        foundation::Function<void()> m_onActivated;
        foundation::Function<void(bool)> m_onToggled;
    };

    class Menu; // for MenuSubItem's submenu pointer

    // === Submenu item ==========================================================
    // An item that owns a nested Menu, shown to its right on hover or click. It never closes the
    // chain itself; hovering/clicking it asks the owning menu to open the submenu (wired by the
    // menu when the item is added).
    class MenuSubItem : public MenuItem
    {
        DRACONIC_OBJECT(MenuSubItem, MenuItem)
    public:
        MenuSubItem() { SetTag(foundation::StringView(u8"menusubmenu")); }

        void SetSubMenu(Menu* menu) noexcept { m_subMenu = menu; }
        [[nodiscard]] Menu* GetSubMenu() const noexcept { return m_subMenu; }

        // The menu installs this to open the submenu (also used by hover).
        void SetOnOpenSubMenu(foundation::Function<void()> callback) { m_onOpen = foundation::Move(callback); }

        [[nodiscard]] Color ArrowColor() const noexcept { return m_arrowColor; }

    protected:
        void Activate() override
        {
            if (m_onOpen)
                m_onOpen();
        } // open, never close

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            MenuItem::OnDraw(ctx, localBounds);
            // A right-pointing arrow in the right pad.
            const Rect b = GetLocalBounds();
            const f32 aw = 5.0f, ah = 8.0f;
            const f32 ax = b.x + b.width - aw - 6.0f;
            const f32 ay = b.y + (b.height - ah) * 0.5f;
            vg::PathBuilder pb;
            pb.MoveTo(ax, ay);
            pb.LineTo(ax, ay + ah);
            pb.LineTo(ax + aw, ay + ah * 0.5f);
            pb.Close();
            ctx.VG().FillPath(pb.ToPath(), m_arrowColor);
        }

    private:
        Menu* m_subMenu = nullptr; // non-owning; owned as a RefPtr by the Menu
        foundation::Function<void()> m_onOpen;
        Color m_arrowColor{0.80f, 0.84f, 0.90f, 1.0f};
    };

    // === Menu ==================================================================
    class Menu : public UIWidget
    {
        DRACONIC_OBJECT(Menu, UIWidget)
    public:
        Menu()
        {
            SetTag(foundation::StringView(u8"menu"));
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_panelColor));
        }

        void SetFont(fonts::CachedFont* font)
        {
            m_font = font;
            for (MenuRow* row : m_rows)
                if (MenuItem* it = foundation::Cast<MenuItem>(row))
                    it->SetFont(font);
        }
        void SetWidth(f32 width)
        {
            m_width = foundation::Max(1.0f, width);
            m_autoWidth = false;
            Relayout();
        }
        void SetItemHeight(f32 height)
        {
            m_itemHeight = foundation::Max(1.0f, height);
            ApplyItemHeights();
            Relayout();
        }

        // Auto-size the menu width to its widest item (default off; SetWidth turns it off).
        void SetAutoWidth(bool enabled)
        {
            m_autoWidth = enabled;
            Relayout();
        }

        // Add a plain activatable item. Activating runs `action`, then closes the whole chain.
        MenuItem* AddItem(foundation::StringView text, foundation::Function<void()> action)
        {
            auto item = foundation::MakeRef<MenuItem>(foundation::DefaultAllocator());
            item->SetText(text);
            foundation::Function<void()> act = foundation::Move(action);
            item->SetOnPicked(
                [act = foundation::Move(act)]()
                {
                    if (act)
                        act();
                });
            MenuItem* raw = item.Get();
            AddItemNode(item.Get(), raw);
            return raw;
        }

        // Add a checkable item. Activating toggles it (firing `onToggled`) and closes the chain.
        MenuItem* AddCheckItem(foundation::StringView text, bool checked,
                               foundation::Function<void(bool)> onToggled)
        {
            auto item = foundation::MakeRef<MenuItem>(foundation::DefaultAllocator());
            item->SetText(text);
            item->SetCheckable(true);
            item->SetCheckedSilently(checked);
            item->SetOnToggled(foundation::Move(onToggled));
            MenuItem* raw = item.Get();
            AddItemNode(item.Get(), raw);
            return raw;
        }

        // Add a horizontal separator (a non-interactive divider).
        void AddSeparator()
        {
            auto sep = foundation::MakeRef<MenuSeparator>(foundation::DefaultAllocator());
            MenuSeparator* raw = sep.Get();
            Menu* self = this;
            raw->SetOnHovered([self, raw]() { self->OnRowHovered(raw); });
            AddChild(sep.Get());
            m_rows.PushBack(raw);
            Relayout();
        }

        // Add a submenu item; returns the (empty) child Menu to populate. The child is owned by
        // this menu and shown to the item's right on hover/click.
        Menu* AddSubMenu(foundation::StringView text)
        {
            auto item = foundation::MakeRef<MenuSubItem>(foundation::DefaultAllocator());
            item->SetText(text);
            auto sub = foundation::MakeRef<Menu>(foundation::DefaultAllocator());
            sub->SetFont(m_font);
            sub->SetItemHeight(m_itemHeight);
            item->SetSubMenu(sub.Get());

            MenuSubItem* rawItem = item.Get();
            Menu* rawSub = sub.Get();
            Menu* self = this;
            item->SetOnOpenSubMenu([self, rawItem]() { self->OpenSubMenuFor(rawItem); });

            AddItemNode(item.Get(), rawItem);
            m_ownedSubMenus.PushBack(foundation::Move(sub)); // keep the submenu alive
            return rawSub;
        }

        [[nodiscard]] usize ItemCount() const noexcept
        {
            usize n = 0;
            for (MenuRow* row : m_rows)
                if (row->IsSelectable())
                    ++n;
            return n;
        }
        [[nodiscard]] usize RowCount() const noexcept { return m_rows.Size(); }
        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
        [[nodiscard]] Menu* CurrentSubMenu() const noexcept { return m_currentSubMenu; }

        // Notified when this menu closes as a top-level popup (outside click / Escape /
        // activation). A MenuBar uses it to clear its "open menu" state.
        void SetOnClosed(foundation::Function<void()> callback) { m_onClosed = foundation::Move(callback); }

        // Close this menu if it is open as a popup (used by a MenuBar toggle).
        void Close()
        {
            if (m_open)
                CloseSelf();
        }

        // Show the menu at `position` (root-local) as a popup, attached under `owner`'s root.
        // `popupOwner` (optional) is a node whose clicks do NOT dismiss the menu - a MenuBar
        // passes its button so clicking it again can toggle the menu closed. A context menu
        // passes none, so ANY press outside the menu's whole open chain dismisses it. The
        // chain-aware `contains` predicate keeps clicks inside open submenus from dismissing.
        void Open(Node& owner, foundation::Float2 position, Node* popupOwner = nullptr)
        {
            Node* root = owner.GetRootNode();
            EventDispatcher* dispatcher = owner.GetEventDispatcher();
            if (root == nullptr || dispatcher == nullptr)
                return;
            if (m_open)
            {
                SetPosition(position);
                return;
            } // already open: just move it
            SetPosition(position);
            root->AddChild(this);
            m_open = true;
            Menu* self = this;
            dispatcher->OpenPopup(
                this, popupOwner, [self]() { self->OnClosed(); },
                [self](Node* n) { return self->ChainContains(n); });
        }

    protected:
        void OnSizeChange() override { Relayout(); }

    private:
        void AddItemNode(Node* node, MenuRow* row)
        {
            if (MenuItem* it = foundation::Cast<MenuItem>(row))
            {
                it->SetFont(m_font);
                it->SetTextColor(m_textColor);
                it->SetRowHeight(m_itemHeight);
                Menu* self = this;
                it->SetOnActivated([self]() { self->RootMenu()->CloseSelf(); });
            }
            Menu* self = this;
            row->SetOnHovered([self, row]() { self->OnRowHovered(row); });
            AddChild(node);
            m_rows.PushBack(row);
            Relayout();
        }

        void ApplyItemHeights()
        {
            for (MenuRow* row : m_rows)
                if (MenuItem* it = foundation::Cast<MenuItem>(row))
                    it->SetRowHeight(m_itemHeight);
        }

        void Relayout()
        {
            f32 width = m_width;
            if (m_autoWidth)
            {
                f32 widest = 1.0f;
                for (MenuRow* row : m_rows)
                    if (MenuItem* it = foundation::Cast<MenuItem>(row))
                        widest = foundation::Max(widest, it->PreferredWidth());
                width = widest;
            }

            f32 y = 0.0f;
            for (MenuRow* row : m_rows)
            {
                const f32 h = row->RowHeight();
                row->SetPosition(foundation::Float2{0.0f, y});
                row->SetSize(foundation::Float2{width, h});
                y += h;
            }
            SetSize(foundation::Float2{width, y});
        }

        // === Submenu chain ===
        void OpenSubMenuFor(MenuSubItem* item)
        {
            Menu* sub = item->GetSubMenu();
            if (sub == nullptr || sub == m_currentSubMenu)
                return; // already open
            if (m_currentSubMenu != nullptr)
            {
                m_currentSubMenu->HideAsSubMenu();
                m_currentSubMenu = nullptr;
            }

            Node* root = GetRootNode();
            if (root == nullptr)
                return;
            // Assume identity root transforms (menus attach directly to the root): the item's
            // root-local y = this menu's position + the item's local position.
            const foundation::Float2 pos{GetPosition().x + GetSize().x,
                                   GetPosition().y + item->GetPosition().y};
            sub->m_ownerItem = item;
            sub->SetPosition(pos);
            root->AddChild(sub);
            sub->m_open = true;
            m_currentSubMenu = sub;
        }

        // Hide this menu when shown as a submenu (recursively hides its own open submenus).
        void HideAsSubMenu()
        {
            if (m_currentSubMenu != nullptr)
            {
                m_currentSubMenu->HideAsSubMenu();
                m_currentSubMenu = nullptr;
            }
            m_open = false;
            m_ownerItem = nullptr;
            RemoveFromParent();
        }

        // A row was hovered: close the current submenu unless the hovered row owns it, then open
        // this row's submenu if it is a submenu item.
        void OnRowHovered(MenuRow* row)
        {
            if (m_currentSubMenu != nullptr && m_currentSubMenu->m_ownerItem != row)
            {
                m_currentSubMenu->HideAsSubMenu();
                m_currentSubMenu = nullptr;
            }
            if (MenuSubItem* sub = foundation::Cast<MenuSubItem>(row))
                OpenSubMenuFor(sub);
        }

        // The root of the open chain (walk owner links up).
        Menu* RootMenu()
        {
            Menu* menu = this;
            while (menu->m_ownerItem != nullptr)
            {
                Menu* parent = foundation::Cast<Menu>(menu->m_ownerItem->GetParent());
                if (parent == nullptr)
                    break;
                menu = parent;
            }
            return menu;
        }

        // True if `node` is inside this menu or any menu in its open submenu chain.
        [[nodiscard]] bool ChainContains(Node* node) const
        {
            if (IsInSubtree(node, this))
                return true;
            return m_currentSubMenu != nullptr && m_currentSubMenu->ChainContains(node);
        }

        void CloseSelf()
        {
            if (EventDispatcher* d = GetEventDispatcher())
                d->ClosePopup();
            else
                OnClosed();
        }
        void OnClosed()
        {
            m_open = false;
            if (m_currentSubMenu != nullptr)
            {
                m_currentSubMenu->HideAsSubMenu();
                m_currentSubMenu = nullptr;
            }
            RemoveFromParent();
            if (m_onClosed)
                m_onClosed();
        }

        [[nodiscard]] static bool IsInSubtree(Node* node, const Node* ancestor)
        {
            for (Node* n = node; n != nullptr; n = n->GetParent())
                if (n == ancestor)
                    return true;
            return false;
        }

        Array<MenuRow*> m_rows;                    // rows, owned as children
        Array<foundation::RefPtr<Menu>> m_ownedSubMenus; // keep submenus alive (they live under the root)
        MenuSubItem* m_ownerItem = nullptr;        // set when shown AS a submenu
        Menu* m_currentSubMenu = nullptr;          // the open child submenu (non-owning; a sibling)
        fonts::CachedFont* m_font = nullptr;
        bool m_open = false;
        bool m_autoWidth = false;
        f32 m_width = 160.0f;
        f32 m_itemHeight = 26.0f;
        Color m_panelColor{0.16f, 0.17f, 0.21f, 1.0f};
        Color m_textColor{0.88f, 0.90f, 0.94f, 1.0f};
        foundation::Function<void()> m_onClosed;
    };

    DRACONIC_DEFINE_OBJECT(MenuRow, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(MenuSeparator, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(MenuItem, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(MenuSubItem, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(Menu, "draconic::gui")
}
