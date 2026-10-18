// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :menu partition
//
// Menu (PopupMenu): a floating vertical list of rows - a context menu or a menu dropped from a
// menu bar. Modeled on eepp's UIMenu/UIPopUpMenu family (role only). Open() shows it as an auto
// popup (E5), a child of what opened it, in the top layer, so a press outside or Escape closes it.
// Rows come in a small hierarchy mirroring eepp:
//   MenuRow          - base row (height + hover-notify hook)
//     MenuSeparator  - a non-interactive divider line
//     MenuItem       - an activatable row: text + optional check mark / icon / shortcut column
//       MenuSubItem  - an item that opens a nested submenu on hover or click
//
// A submenu is a popup of its item: open, it is the item's child, so the menus holding it are its
// ancestors and the popup stack's nesting rules give the chain for free (a press inside any open
// menu of the chain closes only the menus deeper than it; opening another item's submenu closes
// the open one; closing a menu closes what it holds). It is placed right of its item, flipped
// left at the root's edge. Activating a leaf item closes the whole chain from its root menu.
// While the pointer heads for an open submenu across other rows (inside the triangle from where
// it was to the submenu's near edge, Godot's safe area), the submenu stays open; if the pointer
// rests there a moment, the row under it takes over.
//
// In markup a Menu holds MenuItems (`text`, `shortcut`, `checkable`, `checked`, a `command`),
// MenuSeparators and nested Menus (its `text` the submenu item's); an item reports `activate`.
// A menu is not shown until opened: one in a page waits there for a `context-menu` naming it.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:menu;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, Max, Cast
import foundation.fonts; // CachedFont
import foundation.vg;    // PathBuilder, StrokeStyle
import :rect;
import :event;
import :draw_context;
import :drawable;
import :rectangle_drawable;
import :text;
import :node;
import :ui_widget;
import :event_dispatcher; // the popup stack (E5)
import :actions;          // TickAction (the safe area's grace)
import :image_drawable;   // a command's icon
import :resource_provider;
import foundation.image;  // ImageData
import :popup_placement;
import :model_index; // ItemEvent (an item's `activate`)

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;
namespace image = foundation::image;

export namespace experimental::gui
{
    // === Row base ===============================================================
    // A row in a menu. Reports its preferred height and, on hover, notifies the owning menu
    // (which uses it to open/close submenus). Selectable rows are the ones keyboard navigation
    // and activation consider.
    class MenuRow : public UIWidget
    {
        RTTI_OBJECT(MenuRow, UIWidget)
    public:
        [[nodiscard]] virtual f32 RowHeight() const = 0;
        [[nodiscard]] virtual bool IsSelectable() const { return false; }

        // Called (with this row) whenever the pointer enters it - the menu wires this.
        void SetOnHovered(core::Function<void()> callback) { m_onHovered = core::Move(callback); }

    protected:
        void OnMouseEnter(const MouseEvent& event) override
        {
            (void)event;
            if (m_onHovered)
                m_onHovered();
        }

    private:
        core::Function<void()> m_onHovered;
    };

    // === Separator =============================================================
    class MenuSeparator : public MenuRow
    {
        RTTI_OBJECT(MenuSeparator, MenuRow)
    public:
        MenuSeparator() { SetTag(core::StringView(u8"menuseparator")); }

        [[nodiscard]] f32 RowHeight() const override { return m_height; }
        void SetLineColor(Color color)
        {
            m_line = color;
            Invalidate();
        }

        // Theming part: menuseparator::line.
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"line"));
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetLocalBounds();
            const f32 y = b.y + b.height * 0.5f;
            GetPartStyle(u8"line").Draw(ctx, Rect{b.x + 8.0f, y, core::Max(0.0f, b.width - 16.0f), 1.0f}, FilledBox(m_line));
        }

    private:
        f32 m_height = 9.0f;
        Color m_line{0.32f, 0.34f, 0.40f, 1.0f};
    };

    // === Activatable item ======================================================
    class MenuItem : public MenuRow
    {
        RTTI_OBJECT(MenuItem, MenuRow)
    public:
        MenuItem()
        {
            SetTag(core::StringView(u8"menuitem"));
            SetTabFocusable(true); // the keyboard and the pad walk the items (a disabled one is skipped)
            m_text.SetAlignment(TextHAlign::Start, TextVAlign::Middle);
            m_shortcut.SetAlignment(TextHAlign::End, TextVAlign::Middle);
        }

        // Content.
        void SetText(core::StringView text)
        {
            m_text.SetString(text);
            Invalidate();
        }
        [[nodiscard]] core::StringView GetText() const { return m_text.GetString(); }
        void SetShortcut(core::StringView text)
        {
            m_shortcut.SetString(text);
            Invalidate();
        }
        [[nodiscard]] core::StringView GetShortcut() const { return m_shortcut.GetString(); }
        void SetIcon(core::RefPtr<Drawable> icon)
        {
            m_icon = core::Move(icon);
            Invalidate();
        }

        void SetHighlightColor(Color color) { m_highlight = color; }
        void SetRowHeight(f32 height) { m_rowHeight = core::Max(1.0f, height); }

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
            MarkStyleStateDirty(StateChecked);
            Invalidate();
            if (m_onToggled)
                m_onToggled(m_checked);
        }
        [[nodiscard]] bool IsChecked() const noexcept { return m_checked; }
        [[nodiscard]] u32 GetWidgetStates() const override { return m_checkable && m_checked ? WidgetStateChecked : WidgetStateNone; }
        void SetCheckedSilently(bool checked)
        {
            if (checked != m_checked)
                MarkStyleStateDirty(StateChecked);
            m_checked = checked;
            Invalidate();
        } // no callback (init)
        void OnBoundCommandState(bool available, bool checked) override
        {
            SetEnabled(available);
            if (m_checkable)
                SetCheckedSilently(checked);
        }
        void SetOnToggled(core::Function<void(bool)> callback)
        {
            m_onToggled = core::Move(callback);
        }
        // An item bound to a command shows its label where it has no text of its own, its keys
        // where it has no shortcut of its own, and its icon.
        void OnBoundCommandText(core::StringView label, core::StringView icon, core::StringView shortcut) override
        {
            if (GetText().Size() == 0 || m_textFromCommand)
            {
                SetText(label);
                m_textFromCommand = true;
            }
            if (GetShortcut().Size() == 0 || m_shortcutFromCommand)
            {
                SetShortcut(shortcut);
                m_shortcutFromCommand = true;
            }
            if (icon.Size() != 0)
                if (IResourceProvider* resources = GetResourceProvider())
                    if (const image::ImageData* image = resources->LoadImage(icon))
                        SetIcon(core::MakeRef<ImageDrawable>(MemoryAllocator(), image));
        }

        // Markup: its text and shortcut exactly as written.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"text"))
                SetText(value);
            else if (name == core::StringView(u8"shortcut"))
                SetShortcut(value);
            else
                return MenuRow::SetMarkupAttribute(name, value);
            return true;
        }
        [[nodiscard]] core::String GetTextString() const { return core::String(GetText()); }
        void SetTextString(const core::String& text) { SetText(text.AsView()); }

        // The user's action (run before the menu closes).
        void SetOnPicked(core::Function<void()> callback) { m_onPicked = core::Move(callback); }
        // The menu installs this: its text style changed (its width may have).
        void SetOnTextStyleChanged(core::Function<void()> callback) { m_onTextStyleChanged = core::Move(callback); }
        // The menu installs this to close the whole chain after activation.
        void SetOnActivated(core::Function<void()> callback)
        {
            m_onActivated = core::Move(callback);
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
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"highlight"));
        }

    protected:
        // Its label and shortcut draw in its text style.
        void OnTextStyleChange() override
        {
            m_text.SetFont(GetFont());
            m_shortcut.SetFont(GetFont());
            m_text.SetDirection(TextDirectionFor());
            m_shortcut.SetDirection(TextDirectionFor());
            m_text.SetColor(GetTextColor());
            m_shortcut.SetColor(GetTextColor());
            if (m_onTextStyleChanged)
                m_onTextStyleChanged(); // its menu measures again (a font that arrives widens it)
        }

        void OnMouseClick(const MouseEvent& event) override
        {
            event.Handled = true;
            Activate();
        }
        // Return, Space, a pad's South: as a click.
        bool OnActivate() override
        {
            Activate();
            return true;
        }

        // Run the item: toggle (if checkable), fire the user's action, then close the chain.
        virtual void Activate()
        {
            if (HasCommand())
                (void)RunBoundCommand(); // a bound command decides its own checked state (G7)
            else if (m_checkable)
                SetChecked(!m_checked);
            if (m_onPicked)
                m_onPicked();
            // A script's `activate`: it bubbles (through the menu to what opened it), so one
            // listener hears a whole menu's items, the event's id saying which.
            ItemEvent activated(EventType::ItemActivate, this, -1, ModelIndex{});
            (void)RouteEvent(activated);
            if (m_onActivated)
                m_onActivated();
        }

        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            (void)localBounds;
            const Rect b = GetLocalBounds();
            if (IsLit())
                GetPartStyle(u8"highlight").Draw(ctx, b, FilledBox(m_highlight));

            // Left gutter: a check mark (if checked) else an icon (if set).
            if (m_checkable && m_checked)
            {
                DrawCheck(ctx, b);
            }
            else if (m_icon)
            {
                const f32 s = core::Max(0.0f, core::Max(0.0f, kGutter - 8.0f));
                m_icon->Draw(ctx, Rect{b.x + 4.0f, b.y + (b.height - s) * 0.5f, s, s});
            }

            // Text + shortcut share the inner rect (opposite alignments).
            const Rect inner{b.x + kGutter, b.y, core::Max(0.0f, b.width - kGutter - kRightPad),
                             b.height};
            m_text.Draw(ctx, inner);
            if (m_shortcut.GetString().Size() > 0)
                m_shortcut.Draw(ctx, inner);
        }

        /// Lit: under the pointer, focused, or (a submenu item) with its submenu open.
        [[nodiscard]] virtual bool IsLit() const { return IsHovered() || IsFocused(); }

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
        core::RefPtr<Drawable> m_icon;
        bool m_checkable = false;
        bool m_checked = false;
        f32 m_rowHeight = 26.0f;
        Color m_highlight{0.24f, 0.40f, 0.62f, 1.0f};
        Color m_checkColor{0.88f, 0.90f, 0.94f, 1.0f};
        core::Function<void()> m_onPicked;
        core::Function<void()> m_onActivated;
        core::Function<void(bool)> m_onToggled;
        core::Function<void()> m_onTextStyleChanged;
        bool m_textFromCommand = false;     // its text, its shortcut: its command's (OnBoundCommandText)
        bool m_shortcutFromCommand = false;
    };

    class Menu; // for MenuSubItem's submenu pointer

    // === Submenu item ==========================================================
    // An item that owns a nested Menu, shown to its right on hover or click. It never closes the
    // chain itself; hovering/clicking it asks the owning menu to open the submenu (wired by the
    // menu when the item is added).
    class MenuSubItem : public MenuItem
    {
        RTTI_OBJECT(MenuSubItem, MenuItem)
    public:
        MenuSubItem() { SetTag(core::StringView(u8"menusubmenu")); }

        void SetSubMenu(Menu* menu) noexcept { m_subMenu = menu; }
        [[nodiscard]] Menu* GetSubMenu() const noexcept { return m_subMenu; }

        // The menu installs this to open the submenu; true from the keys (its first item then takes focus).
        void SetOnOpenSubMenu(core::Function<void(bool)> callback) { m_onOpen = core::Move(callback); }

        [[nodiscard]] Color ArrowColor() const noexcept { return m_arrowColor; }
        /// Lit while its submenu is open too (focus has gone into it).
        [[nodiscard]] bool IsLit() const override;

    protected:
        // A click opens it; Return, Space or Right open it and go in (its first item takes focus).
        void Activate() override
        {
            if (m_onOpen)
                m_onOpen(false);
        } // open, never close
        bool OnActivate() override
        {
            if (m_onOpen)
                m_onOpen(true);
            return true;
        }
        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Target == this && event.KeyCode == static_cast<u32>(KeyCode::Right) && event.Modifiers == 0)
            {
                event.Handled = true;
                (void)OnActivate();
            }
        }

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
        core::Function<void(bool)> m_onOpen;
        Color m_arrowColor{0.80f, 0.84f, 0.90f, 1.0f};
    };

    // === Menu ==================================================================
    class Menu : public UIWidget
    {
        RTTI_OBJECT(Menu, UIWidget)
    public:
        Menu()
        {
            SetTag(core::StringView(u8"menu"));
            // Open, it takes focus (itself: no item lit until a key or the pointer picks one), and
            // up and down go round its items.
            SetTabFocusable(true);
            SetNavScope(NavScope::Wrap);
            SetDisplayed(false); // shown when opened (one in a page waits there, unseen)
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_panelColor));
            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetTextColor(Color{0.88f, 0.90f, 0.94f, 1.0f});
        }

        void SetWidth(f32 width)
        {
            m_width = core::Max(1.0f, width);
            m_autoWidth = false;
            Relayout();
        }
        void SetItemHeight(f32 height)
        {
            m_itemHeight = core::Max(1.0f, height);
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
        MenuItem* AddItem(core::StringView text, core::Function<void()> action)
        {
            auto item = core::MakeRef<MenuItem>(MemoryAllocator());
            item->SetText(text);
            core::Function<void()> act = core::Move(action);
            item->SetOnPicked(
                [act = core::Move(act)]()
                {
                    if (act)
                        act();
                });
            MenuItem* raw = item.Get();
            AddItemNode(item.Get(), raw);
            return raw;
        }

        // Add a checkable item. Activating toggles it (firing `onToggled`) and closes the chain.
        MenuItem* AddCheckItem(core::StringView text, bool checked,
                               core::Function<void(bool)> onToggled)
        {
            auto item = core::MakeRef<MenuItem>(MemoryAllocator());
            item->SetText(text);
            item->SetCheckable(true);
            item->SetCheckedSilently(checked);
            item->SetOnToggled(core::Move(onToggled));
            MenuItem* raw = item.Get();
            AddItemNode(item.Get(), raw);
            return raw;
        }

        // Add a horizontal separator (a non-interactive divider).
        void AddSeparator() { AdoptSeparator(core::MakeRef<MenuSeparator>(MemoryAllocator())); }
        void AdoptSeparator(core::RefPtr<MenuSeparator> sep)
        {
            MenuSeparator* raw = sep.Get();
            Menu* self = this;
            raw->SetOnHovered([self, raw]() { self->OnRowHovered(raw); });
            AddChild(sep.Get());
            m_rows.PushBack(raw);
            Relayout();
        }

        // Add a submenu item; returns the (empty) child Menu to populate. The child is owned by
        // this menu and shown to the item's right on hover/click.
        Menu* AddSubMenu(core::StringView text) { return AdoptSubMenu(core::MakeRef<Menu>(MemoryAllocator()), text); }
        /// Takes `sub` as the submenu of a new item titled `text` (markup's nested Menu).
        Menu* AdoptSubMenu(core::RefPtr<Menu> sub, core::StringView text)
        {
            auto item = core::MakeRef<MenuSubItem>(MemoryAllocator());
            item->SetText(text);
            ShareTextStyle(*sub, false);
            sub->SetItemHeight(m_itemHeight);
            item->SetSubMenu(sub.Get());

            MenuSubItem* rawItem = item.Get();
            Menu* rawSub = sub.Get();
            Menu* self = this;
            item->SetOnOpenSubMenu([self, rawItem](bool fromKeys) { self->OpenSubMenuFor(rawItem, fromKeys); });

            AddItemNode(item.Get(), rawItem);
            m_ownedSubMenus.PushBack(core::Move(sub)); // keep the submenu alive
            return rawSub;
        }

        /// Takes an item made elsewhere (markup's MenuItem).
        void AdoptItem(MenuItem& item) { AddItemNode(&item, &item); }

        /// Its title: a menu bar's button, or a submenu's item (markup's `text`).
        void SetTitle(core::StringView title) { m_title = core::String(title); }
        [[nodiscard]] core::StringView GetTitle() const noexcept { return m_title.AsView(); }
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name != core::StringView(u8"text"))
                return UIWidget::SetMarkupAttribute(name, value);
            SetTitle(value);
            return true;
        }
        /// Markup's children are its items; a menu given items there sizes to its widest.
        [[nodiscard]] Node* GetChildHost() override;
        /// The rows markup gave it, taken in (its MarkupChildHost calls it).
        void AdoptMarkupChildren(Node& host);

        [[nodiscard]] usize ItemCount() const noexcept
        {
            usize n = 0;
            for (MenuRow* row : m_rows)
                if (row->IsSelectable())
                    ++n;
            return n;
        }
        [[nodiscard]] usize RowCount() const noexcept { return m_rows.Size(); }
        [[nodiscard]] bool IsOpen() const
        {
            const EventDispatcher* d = GetEventDispatcher();
            return d != nullptr && d->IsPopupOpen(*this);
        }
        /// The open submenu of one of its items, or null.
        [[nodiscard]] Menu* CurrentSubMenu() const
        {
            for (MenuRow* row : m_rows)
                if (const MenuSubItem* sub = core::Cast<MenuSubItem>(row); sub != nullptr && sub->GetSubMenu() != nullptr &&
                                                                          sub->GetSubMenu()->IsOpen())
                    return sub->GetSubMenu();
            return nullptr;
        }

        // Notified when this menu closes, and why. A MenuBar uses it to clear its "open menu" state.
        void SetOnClosed(core::Function<void(PopupCloseReason)> callback) { m_onClosed = core::Move(callback); }

        // Close this menu (and the submenus it holds) if it is open.
        void Close()
        {
            if (EventDispatcher* d = GetEventDispatcher())
                (void)d->ClosePopup(*this);
        }

        /// Shows the menu as a popup of `owner` at `position` (in the root's space): a context
        /// menu at the pointer, below it and to its right, above it where it does not fit, slid
        /// back in from the edges.
        void Open(Node& owner, core::Float2 position)
        {
            PopupOptions options;
            options.AnchorRect = Rect{position.x, position.y, 0.0f, 0.0f};
            options.HasAnchorRect = true;
            options.TakesFocus = true; // the keyboard and the pad work in it; focus comes back on close
            OpenAs(owner, core::Move(options));
        }
        /// Focuses its first item that can take it (a menu opened from the keyboard or the pad).
        void FocusFirstItem() { FocusItem(0, 1); }
        /// Shows the menu as a popup of `owner`, against `anchor` (a menu bar's button: below it).
        void OpenBelow(Node& owner, Node& anchor)
        {
            PopupOptions options;
            options.Anchor = &anchor;
            options.Placement = PopupPlacement{PopupSide::Below, PopupAlign::Start, 0.0f, true};
            options.TakesFocus = true;
            OpenAs(owner, core::Move(options));
        }

    protected:
        // Its items take its font and colour.
        void OnTextStyleChange() override
        {
            for (MenuRow* row : m_rows)
                if (MenuItem* it = core::Cast<MenuItem>(row))
                    ShareTextStyle(*it);
        }

        void OnSizeChange() override { Relayout(); }

        // Up and down go round the items (from the menu itself: the first or the last); Left in a
        // submenu closes it, focus going back to its item.
        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.Modifiers != 0)
                return;
            const KeyCode key = static_cast<KeyCode>(event.KeyCode);
            if (key == KeyCode::Down || key == KeyCode::Up)
            {
                const i32 step = key == KeyCode::Down ? 1 : -1;
                const i32 at = IndexOfRow(event.Target);
                event.Handled = FocusItem(at < 0 ? (step > 0 ? 0 : static_cast<i32>(m_rows.Size()) - 1) : at + step, step);
            }
            else if (key == KeyCode::Left && core::Cast<MenuSubItem>(GetParent()) != nullptr)
            {
                event.Handled = true;
                Close();
            }
        }

    private:
        // The row `node` is (or is in), or -1.
        [[nodiscard]] i32 IndexOfRow(const Node* node) const
        {
            for (usize i = 0; i < m_rows.Size(); ++i)
                for (const Node* n = node; n != nullptr && n != this; n = n->GetParent())
                    if (n == m_rows[i])
                        return static_cast<i32>(i);
            return -1;
        }
        // Focuses the first item from `start` going `step` (round the end) that can take focus.
        bool FocusItem(i32 start, i32 step)
        {
            const i32 count = static_cast<i32>(m_rows.Size());
            for (i32 n = 0; n < count; ++n)
            {
                const i32 i = ((start + n * step) % count + count) % count;
                MenuRow* row = m_rows[static_cast<usize>(i)];
                if (row->IsSelectable() && row->IsTreeEnabled() && row->IsTabFocusable())
                {
                    row->RequestFocus();
                    return true;
                }
            }
            return false;
        }

        void AddItemNode(Node* node, MenuRow* row)
        {
            if (MenuItem* it = core::Cast<MenuItem>(row))
            {
                ShareTextStyle(*it);
                it->SetRowHeight(m_itemHeight);
                Menu* self = this;
                it->SetOnActivated([self]() { self->RootMenu()->Close(); });
                it->SetOnTextStyleChanged([self]() { self->Relayout(); });
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
                if (MenuItem* it = core::Cast<MenuItem>(row))
                    it->SetRowHeight(m_itemHeight);
        }

        void Relayout()
        {
            f32 width = m_width;
            if (m_autoWidth)
            {
                f32 widest = 1.0f;
                for (MenuRow* row : m_rows)
                    if (MenuItem* it = core::Cast<MenuItem>(row))
                        widest = core::Max(widest, it->PreferredWidth());
                width = widest;
            }

            f32 y = 0.0f;
            for (MenuRow* row : m_rows)
            {
                const f32 h = row->RowHeight();
                row->SetPosition(core::Float2{0.0f, y});
                row->SetSize(core::Float2{width, h});
                y += h;
            }
            SetSize(core::Float2{width, y});
        }

        void OpenAs(Node& owner, PopupOptions options)
        {
            EventDispatcher* dispatcher = owner.GetEventDispatcher();
            if (dispatcher == nullptr || IsOpen())
                return;
            SetDisplayed(true);
            Relayout();
            options.FitContent = true; // sized as it measures, again when a font arrives
            Menu* self = this;
            options.OnClosed = [self](PopupCloseReason reason)
            {
                if (self->m_onClosed)
                    self->m_onClosed(reason);
            };
            dispatcher->OpenPopup(owner, *this, core::Move(options));
        }

        // === Submenus ===
        // Opens `item`'s submenu as its popup, right of it (left at the root's edge); the stack
        // closes another item's open submenu, which this one's owner is not inside. It takes focus
        // (given back to the item when it closes); from the keys its first item is lit.
        void OpenSubMenuFor(MenuSubItem* item, bool fromKeys)
        {
            Menu* sub = item->GetSubMenu();
            if (sub == nullptr)
                return;
            if (sub->IsOpen())
            {
                if (fromKeys)
                    sub->FocusFirstItem();
                return;
            }
            PopupOptions options;
            options.Anchor = item;
            options.Placement = PopupPlacement{PopupSide::Right, PopupAlign::Start, 0.0f, true};
            options.TakesFocus = true;
            sub->OpenAs(*item, core::Move(options));
            if (fromKeys)
                sub->FocusFirstItem();
        }

        // The pointer over it, in the root's space: where it was is the safe area's apex.
        void OnMouseMove(const MouseEvent& event) override
        {
            Node* root = GetRootNode();
            m_pointerAt = root != nullptr ? root->ConvertToNodeSpace(event.Position) : event.Position;
            m_pointerKnown = true;
        }

        // A row was hovered: an open submenu of another row closes (unless the pointer is heading
        // for it: then a moment later, if it rests on this row); a submenu row opens its own; an
        // item takes focus, so the keys go on from where the pointer is.
        void OnRowHovered(MenuRow* row)
        {
            Menu* open = CurrentSubMenu();
            if (open != nullptr && open->GetParent() != row && HeadingFor(*open))
            {
                DeferHover(row);
                return;
            }
            m_deferredRow = nullptr;
            if (open != nullptr && open->GetParent() != row)
                open->Close();
            if (row->IsSelectable() && row->IsTreeEnabled())
                row->RequestFocus();
            if (MenuSubItem* sub = core::Cast<MenuSubItem>(row))
                OpenSubMenuFor(sub, false);
        }

        // Whether the pointer, from where this menu last saw it to where it is now (the hover comes
        // before the move reaches the menu), is inside the triangle from there to the open
        // submenu's near edge.
        [[nodiscard]] bool HeadingFor(const Menu& sub) const
        {
            const EventDispatcher* d = GetEventDispatcher();
            const Node* root = this;
            while (root->GetParent() != nullptr)
                root = root->GetParent();
            if (!m_pointerKnown || d == nullptr)
                return false;
            const core::Float2 apex = m_pointerAt;
            const core::Float2 p = root->ConvertToNodeSpace(d->GetMousePosition());
            const core::Float2 sp = sub.GetPosition();
            const core::Float2 ss = sub.GetSize();
            const f32 edgeX = sp.x >= GetPosition().x + GetSize().x * 0.5f ? sp.x : sp.x + ss.x; // the side facing this menu
            const core::Float2 b{edgeX, sp.y};
            const core::Float2 c{edgeX, sp.y + ss.y};
            const auto side = [](core::Float2 a, core::Float2 b, core::Float2 q) { return (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x); };
            const f32 d1 = side(apex, b, p), d2 = side(b, c, p), d3 = side(c, apex, p);
            const bool negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
            const bool positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
            return !(negative && positive) && (p.x != apex.x || p.y != apex.y);
        }
        // The hover on `row` counts after a moment, if the pointer is still on it and has not
        // reached the submenu.
        void DeferHover(MenuRow* row)
        {
            m_deferredRow = row;
            m_deferredFor = 0.0;
            if (m_deferring || GetActionManager() == nullptr)
                return;
            m_deferring = true;
            Menu* self = this;
            RunAction(core::MakeRef<TickAction>(MemoryAllocator(), [self](core::Duration elapsed)
            {
                if (self->m_deferredRow == nullptr)
                {
                    self->m_deferring = false;
                    return false;
                }
                self->m_deferredFor += elapsed.AsSecondsF();
                if (self->m_deferredFor < kSafeAreaGrace)
                    return true;
                MenuRow* row = self->m_deferredRow;
                self->m_deferredRow = nullptr;
                self->m_deferring = false;
                if (row->IsHovered())
                {
                    self->m_pointerKnown = false; // resting: no heading
                    self->OnRowHovered(row);
                }
                return false;
            }));
        }

        // The outermost menu holding this one (a submenu's parents up the tree).
        Menu* RootMenu()
        {
            Menu* root = this;
            for (Node* n = GetParent(); n != nullptr; n = n->GetParent())
                if (Menu* m = core::Cast<Menu>(n))
                    root = m;
            return root;
        }

        core::String m_title;
        core::RefPtr<Node> m_markupHost;           // where markup's children land, then taken in
        bool m_adopting = false;
        Array<MenuRow*> m_rows;                    // rows, owned as children
        Array<core::RefPtr<Menu>> m_ownedSubMenus; // keep submenus alive (their items' children only while open)
        bool m_autoWidth = false;
        // The safe area toward an open submenu: the pointer's last two places, a hover deferred.
        core::Float2 m_pointerAt{0.0f, 0.0f};
        bool m_pointerKnown = false;
        MenuRow* m_deferredRow = nullptr; // non-owning: one of its rows
        f64 m_deferredFor = 0.0;
        bool m_deferring = false;
        static constexpr f64 kSafeAreaGrace = 0.3; // seconds a resting pointer waits before switching
        f32 m_width = 160.0f;
        f32 m_itemHeight = 26.0f;
        Color m_panelColor{0.16f, 0.17f, 0.21f, 1.0f};
        core::Function<void(PopupCloseReason)> m_onClosed;
    };

    inline Node* Menu::GetChildHost()
    {
        if (!m_markupHost)
        {
            Menu* self = this;
            m_markupHost = core::MakeRef<MarkupChildHost>(MemoryAllocator(), [self](Node& host) { self->AdoptMarkupChildren(host); });
            m_autoWidth = true;
        }
        return m_markupHost.Get();
    }

    inline void Menu::AdoptMarkupChildren(Node& host)
    {
        if (m_adopting)
            return;
        m_adopting = true;
        while (host.ChildCount() != 0)
        {
            core::RefPtr<Node> child(host.GetChildAt(0));
            host.RemoveChild(child.Get());
            if (Menu* sub = core::Cast<Menu>(child.Get()))
                (void)AdoptSubMenu(core::RefPtr<Menu>(sub), sub->GetTitle());
            else if (MenuSeparator* sep = core::Cast<MenuSeparator>(child.Get()))
                AdoptSeparator(core::RefPtr<MenuSeparator>(sep));
            else if (MenuItem* item = core::Cast<MenuItem>(child.Get()))
                AdoptItem(*item);
        }
        m_adopting = false;
    }

    inline bool MenuSubItem::IsLit() const { return MenuItem::IsLit() || (m_subMenu != nullptr && m_subMenu->IsOpen()); }

    RTTI_DEFINE_OBJECT(MenuRow, "rtti::gui")
    RTTI_DEFINE_OBJECT(MenuSubItem, "rtti::gui")
}
