// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :window partition
//
// Window: a movable panel with a title bar and a content area. Dragging the title bar moves the
// window, or, when the press is handed over, starts a drag source's drag. With `resizable`,
// dragging the bottom-right grip resizes the window, clamped to a minimum size. A move or resize
// is reported once the press is released. With `closable`, a close button in the title bar
// closes the window. Pressing anywhere on the title raises the window to the front. The window
// arranges its parts (title bar, content, grip) itself, and the theme styles them
// (window::title, ::grip, `window titlebar > close`).
//
// Movement uses incremental world-space cursor deltas from an internal DragHandle, which captures
// the pointer as Slider and ScrollBar do, with its own m_dragging flag, so a drag that leaves the
// handle keeps tracking. Nested parent transforms are not accounted for: deltas are treated as
// parent-local, which is fine for top-level windows under an untransformed root.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:window;

import foundation.core;  // RefPtr, MakeRef, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :rect;
import :event;
import :drawable;
import :node;
import :label;
import :ui_widget;
import :event_dispatcher;
import :button_base;
import :draw_context;
import :layout_style;
import :commands; // CommandEvent
import :keymap;   // kBackCommand
import foundation.vg; // PathBuilder, StrokeStyle, for the close cross and the grip's lines

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;
namespace vg = foundation::vg;

export namespace foundation::gui
{
    // A widget that reports incremental world-space cursor deltas while dragged. Window uses it for
    // the title bar and the resize grip, but it is generally reusable.
    class DragHandle : public UIWidget
    {
        RTTI_OBJECT(DragHandle, UIWidget)
    public:
        core::Function<void(core::Float2)> OnDrag; // incremental cursor delta
        core::Function<void()> OnPressed;
        core::Function<void()> OnReleased; // the press that dragged it was released, after OnDrag moved something
        /// Sets whether a press drags the handle (the default). When off, the press goes to a drag
        /// source instead (the handle's own, set with SetDragSource, or an ancestor's), as a
        /// floating dock window's title bar does.
        void SetDragsItself(bool drags) noexcept { m_dragsItself = drags; }
        [[nodiscard]] bool DragsItself() const noexcept { return m_dragsItself; }

    protected:
        // A press moves or resizes the window, so no drag is offered to the nodes that hold the handle.
        [[nodiscard]] bool OwnsPressDrag(core::Float2) const override { return m_dragsItself; }
        void OnMouseDown(const MouseEvent& event) override
        {
            event.Handled = true;
            if (OnPressed)
                OnPressed(); // any button raises the window
            if (event.Button != MouseButton::Left || !m_dragsItself)
                return; // only the left button drags
            m_dragging = true;
            m_moved = false;
            m_last = event.Position;
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_dragging)
                return;
            const core::Float2 delta{event.Position.x - m_last.x, event.Position.y - m_last.y};
            m_last = event.Position;
            m_moved = m_moved || delta.x != 0.0f || delta.y != 0.0f;
            if (OnDrag)
                OnDrag(delta);
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            (void)event;
            LetGo();
        }
        void OnPointerCancel(const MouseEvent&) override { LetGo(); }

    private:
        void LetGo()
        {
            const bool moved = m_dragging && m_moved;
            m_dragging = false;
            m_moved = false;
            if (moved && OnReleased)
                OnReleased();
        }
        bool m_dragging = false;
        bool m_moved = false;
        bool m_dragsItself = true;
        core::Float2 m_last{0.0f, 0.0f};
    };

    // A window's close button (`window titlebar > close`): a cross drawn in the button's text colour.
    class WindowCloseButton : public ButtonBase
    {
        RTTI_OBJECT(WindowCloseButton, ButtonBase)
    public:
        WindowCloseButton()
        {
            SetTag(core::StringView(u8"close"));
            SetTabFocusable(false); // from the keyboard, Escape closes the window instead
        }
        [[nodiscard]] core::Float2 MeasureContent(core::Float2) override { return core::Float2{10.0f, 10.0f}; }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            ButtonBase::OnDraw(ctx, localBounds);
            const Rect box = GetContentBounds();
            const f32 r = core::Min(box.width, box.height) * 0.5f;
            const f32 cx = box.x + box.width * 0.5f;
            const f32 cy = box.y + box.height * 0.5f;
            vg::PathBuilder cross;
            cross.MoveTo(cx - r, cy - r);
            cross.LineTo(cx + r, cy + r);
            cross.MoveTo(cx + r, cy - r);
            cross.LineTo(cx - r, cy + r);
            ctx.VG().StrokePath(cross.ToPath(), GetTextColor(), vg::StrokeStyle(1.5f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
        }
        // The button handles its own press, so the title bar underneath does not start a move.
        void OnMouseDown(const MouseEvent& event) override
        {
            if (event.Button == MouseButton::Left)
                event.Handled = true;
        }
    };

    // A window's resize grip: diagonal lines in the corner, in the window::grip part's colour.
    class WindowGrip : public DragHandle
    {
        RTTI_OBJECT(WindowGrip, DragHandle)
    public:
        WindowGrip() { SetTag(core::StringView(u8"grip")); }
        void SetLineColor(Color color)
        {
            m_color = color;
            Invalidate();
        }

    protected:
        void OnDraw(DrawContext& ctx, const Rect& localBounds) override
        {
            DragHandle::OnDraw(ctx, localBounds);
            // Draw the lines in the grip's corner: bottom right, or bottom left in right to left.
            const Rect b = GetLocalBounds();
            const bool rtl = IsRightToLeft();
            vg::PathBuilder lines;
            for (f32 k = 4.0f; k <= 12.0f; k += 4.0f)
            {
                lines.MoveTo(MirroredX(b.x + b.width - 3.0f, b, rtl), b.y + b.height - 3.0f - k);
                lines.LineTo(MirroredX(b.x + b.width - 3.0f - k, b, rtl), b.y + b.height - 3.0f);
            }
            ctx.VG().StrokePath(lines.ToPath(), m_color, vg::StrokeStyle(1.2f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
        }

    private:
        Color m_color{0.55f, 0.59f, 0.66f, 1.0f};
    };

    class Window : public UIWidget
    {
        RTTI_OBJECT(Window, UIWidget)
    public:
        Window()
        {
            SetTag(core::StringView(u8"window"));
            SetClipChildren(true);
            SetBox(FilledBox(m_bodyColor)); // the default box; a style sheet's (rounded, bordered, shadowed) replaces it

            m_titleBar = core::MakeRef<DragHandle>(MemoryAllocator());
            m_titleBar->SetTag(core::StringView(u8"titlebar"));
            {
                StyleWriteScope barDefaults(*m_titleBar);
                m_titleBar->EditLayoutStyle([](LayoutStyle& st)
                {
                    st.Direction = FlexDirection::Row;
                    st.AlignItems = Align::Center;
                    st.ColumnGap = 6.0f;
                });
                // The padding is the same at both ends, so in right to left the bar mirrors itself.
                m_titleBar->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            }
            Window* self = this;
            m_titleBar->OnDrag = [self](core::Float2 d)
            {
                self->SetPosition(
                    core::Float2{self->GetPosition().x + d.x, self->GetPosition().y + d.y});
            };
            m_titleBar->OnPressed = [self]() { self->ToFront(); };
            m_titleBar->OnReleased = [self]() { self->UserArranged(); };
            AddChild(m_titleBar.Get());

            m_title = core::MakeRef<Label>(MemoryAllocator());
            StyleWriteScope defaults(*m_title); // defaults: a style sheet overrides them and falls back to them
            m_title->SetTextAlignment(TextHAlign::Start, TextVAlign::Middle);
            m_title->SetHitTestVisible(false); // clicks fall through to the draggable title bar
            m_title->EditLayoutStyle([](LayoutStyle& st) { st.Grow = 1.0f; });
            m_titleBar->AddChild(m_title.Get());
            m_closeButton = core::MakeRef<WindowCloseButton>(MemoryAllocator());
            m_closeButton->SetOnClick([self]() { self->RequestClose(); });
            m_titleBar->AddChild(m_closeButton.Get());

            m_contentHost = core::MakeRef<UIWidget>(MemoryAllocator());
            m_contentHost->SetClipChildren(true);
            AddChild(m_contentHost.Get());

            m_resizeGrip = core::MakeRef<WindowGrip>(MemoryAllocator());
            m_resizeGrip->OnDrag = [self](core::Float2 d) { self->ResizeBy(d); };
            m_resizeGrip->OnPressed = [self]() { self->ToFront(); };
            m_resizeGrip->OnReleased = [self]() { self->UserArranged(); };
            m_resizeGrip->SetCursor(Cursor::NwseResize);
            AddChild(m_resizeGrip.Get());
            RefreshPartNodes();
        }

        // The area for window content (add your widgets here).
        [[nodiscard]] Node* GetContent() const noexcept { return m_contentHost.Get(); }
        [[nodiscard]] Node* GetChildHost() override { return m_contentHost.Get(); } // markup's children become the content

        void SetTitle(core::StringView text) { m_title->SetText(text); }
        [[nodiscard]] core::StringView GetTitle() const { return m_title->GetText(); }
        void SetMinSize(core::Float2 size) { m_minSize = size; }
        /// Sets whether the close button shows. Clicking it closes the window, as Escape does on a
        /// dialog.
        void SetClosable(bool closable)
        {
            m_closable = closable;
            m_closeButton->SetDisplayed(closable);
        }
        [[nodiscard]] bool IsClosable() const noexcept { return m_closable; }
        /// Sets whether the resize grip shows and resizes the window.
        void SetResizable(bool resizable)
        {
            m_resizable = resizable;
            m_resizeGrip->SetDisplayed(resizable);
        }
        [[nodiscard]] bool IsResizable() const noexcept { return m_resizable; }
        /// Called when the window is asked to close, by its close button or by `back` while open as a
        /// modal. A plain window closes; a dialog answers as its cancel button does.
        virtual void RequestClose() { Close(); }
        /// Returns whether `back` (Escape, the pad's East button) asks the window to close while it
        /// is open as a modal. By default that is when the window is closable.
        [[nodiscard]] virtual bool AnswersBack() const { return m_closable; }
        [[nodiscard]] f32 GetTitleBarHeight() const noexcept { return m_titleBarHeight; }
        void SetTitleBarHeight(f32 height)
        {
            m_titleBarHeight = core::Max(1.0f, height);
            Relayout();
        }

        void SetBodyColor(Color color)
        {
            m_bodyColor = color;
            if (!IsBoxFromStyle())
                SetBox(FilledBox(color));
        }
        void SetTitleColor(Color color)
        {
            m_titleColor = color;
            RefreshPartNodes();
        }

        // Fired when the window is closed via Close().
        void SetOnClose(core::Function<void()> callback) { m_onClose = core::Move(callback); }
        /// Sets the callback for the user moving the window by its title bar or resizing it by its
        /// grip. It runs once when the press is released, not on each move, and the window's
        /// position and size are final by then.
        void SetOnUserArranged(core::Function<void()> callback) { m_onUserArranged = core::Move(callback); }
        /// Returns the title bar, which moves the window when dragged. DragHandle::SetDragsItself
        /// hands the press to a drag source instead.
        [[nodiscard]] DragHandle* GetTitleBar() const noexcept { return m_titleBar.Get(); }

        // Shows the window centred in `owner` as a modal window: a manual, modal popup of `owner`.
        // Everything below it is inert (pointer, keys, bindings) until Close(), its `::backdrop`
        // part dims the root behind it, and Tab stays within it. Focus moves to its first focusable
        // child and returns when it closes. Popups opened inside it (a combo box's list) stack
        // above it.
        void OpenModal(Node& owner) { OpenInternal(owner, /*modal*/ true); }

        // Shows the window centred in `root` as a normal, non-blocking top-level window.
        void Open(Node& root) { OpenInternal(root, /*modal*/ false); }

        // Removes the window and calls the close callback. A modal window is closed through the popup
        // stack, which gives focus back. A window placed in a tree as a child without being opened
        // closes the same way, so its close button works however it came to be shown.
        void Close()
        {
            if (!m_open && GetParent() == nullptr)
                return;
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (m_modal && dispatcher != nullptr && dispatcher->IsPopupOpen(*this))
            {
                (void)dispatcher->ClosePopup(*this); // the popup's OnClosed finishes the close
                return;
            }
            const core::RefPtr<Node> keep(this); // the tree may hold the only reference (as for a script's window)
            RemoveFromParent();
            Closed();
            OnWindowGone();
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }

        // === The script surface (reflected in GUIReflectionImpl.cpp) ===
        /// Opens the window where it is, over its parent (for a window a script added to a page).
        /// Returns false when it has no parent.
        bool OpenInPlace()
        {
            Node* parent = GetParent();
            if (parent == nullptr)
                return false;
            SetDisplayed(true); // a window placed in a page stays out of the layout until opened
            Open(*parent);
            return true;
        }
        /// Opens the window as a modal over the GUI it is in. Returns false when it is in no GUI.
        bool OpenModalOver()
        {
            Node* root = GetRootNode();
            if (root == nullptr || root == this)
                return false;
            OpenModal(*root);
            return true;
        }
        [[nodiscard]] bool IsModal() const noexcept { return m_modal; }

        // Style parts: window::title (the header bar) and ::grip (the resize handle).
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"title"));
            out.PushBack(core::StringView(u8"grip"));
        }
        // The title bar and the grip are separate nodes, so the window::title and ::grip parts give
        // them their boxes, over their default colours.
        void OnPartStyleChange(core::StringView part) override
        {
            if (part == core::StringView(u8"title") || part == core::StringView(u8"grip"))
                RefreshPartNodes();
        }

    protected:
        /// Called when the window has closed. IsOpen is false, though a modal window may still be
        /// playing its exit.
        virtual void OnWindowClosed() {}
        /// Called when the window was about to open as a modal and its dispatcher refused
        /// (SetModalInterceptor). It answers as closing would, without having been shown; a dialog
        /// answers as its cancel button does.
        virtual void OnModalRefused()
        {
            if (m_onClose)
                m_onClose();
            SendEvent(Event(EventType::Close, this));
        }
        /// Called when the window has gone from the top layer after its exit; a dialog written in a
        /// page goes back there at this point. It is not called when the window opens again before
        /// its exit ends.
        virtual void OnWindowGone() {}
        void OnSizeChange() override { Relayout(); }
        // Places the parts: the title bar across the top, the content below it, the grip in the corner.
        void OnLayout() override { Relayout(); }
        // The title uses the window's font; with a sheet, inheritance gives the same.
        void OnTextStyleChange() override { ShareTextStyle(*m_title, false); }

    private:
        // Updates the title bar's and the grip's boxes: their default colours, with the window::title
        // and ::grip part styles over them.
        void RefreshPartNodes()
        {
            const PartStyle& title = GetPartStyle(u8"title");
            const PartStyle& grip = GetPartStyle(u8"grip");
            m_titleBar->SetBox(title.Over(FilledBox(m_titleColor)), false);
            m_resizeGrip->SetLineColor(grip.HasBackground ? grip.Box.BackgroundColor : m_gripColor); // the grip is drawn as lines
            StyleWriteScope titleWrite(*m_titleBar);
            m_titleBar->SetAlpha(title.Opacity);
            StyleWriteScope gripWrite(*m_resizeGrip);
            m_resizeGrip->SetAlpha(grip.Opacity);
        }
        void OpenInternal(Node& owner, bool modal)
        {
            // Never open inside itself or its own descendants, which would make the tree loop.
            for (const Node* n = &owner; n != nullptr; n = n->GetParent())
                if (n == this)
                    return;
            if (m_open)
                return;
            EventDispatcher* dispatcher = owner.GetEventDispatcher();
            if (modal && dispatcher != nullptr)
            {
                if (!dispatcher->AllowsModal(*this))
                {
                    OnModalRefused(); // never shown, so answer as cancel would
                    return;
                }
                m_modal = true;
                m_open = true;
                CenterIn(owner.GetRootNode() != nullptr ? *owner.GetRootNode() : owner); // centre in the root's space
                PopupOptions options;
                options.Kind = PopupKind::Manual;
                options.Modal = true;
                options.TakesFocus = true;
                Window* self = this;
                options.OnClosed = [self](PopupCloseReason) { self->Closed(); };
                // On close, play the sheet's exit (a dialog fades out) before the window goes.
                options.LeavesOnClose = true;
                options.OnGone = [self]() { self->OnWindowGone(); };
                // While open, `back` asks the window to close. Everything under it is inert, so nothing
                // else would receive `back`.
                if (AnswersBack())
                    m_backHandler = OnCommand(core::StringView(kBackCommand), [self](CommandEvent&) { self->RequestClose(); });
                dispatcher->OpenPopup(owner, *this, core::Move(options));
                return;
            }
            m_modal = false;
            owner.AddChild(this);
            CenterIn(owner);
            ToFront();
            m_open = true;
        }
        void UserArranged()
        {
            if (m_onUserArranged)
                m_onUserArranged();
        }
        void Closed()
        {
            m_open = false;
            m_modal = false;
            if (m_backHandler != 0)
                (void)RemoveCommandHandler(m_backHandler);
            m_backHandler = 0;
            OnWindowClosed();
            if (m_onClose)
                m_onClose();
            SendEvent(Event(EventType::Close, this)); // for a script's on("close")
        }

        void CenterIn(Node& root)
        {
            const core::Float2 r = root.GetSize();
            const core::Float2 s = GetSize();
            SetPosition(core::Float2{(r.x - s.x) * 0.5f, (r.y - s.y) * 0.5f});
        }

        // Resizes from the grip's corner. In right to left the grip is at the bottom left, so the
        // left edge follows the pointer and the right edge stays put.
        void ResizeBy(core::Float2 delta)
        {
            const core::Float2 s = GetSize();
            const bool rtl = IsRightToLeft();
            const f32 width = core::Max(m_minSize.x, s.x + (rtl ? -delta.x : delta.x));
            if (rtl && width != s.x)
                SetPosition(core::Float2{GetPosition().x + s.x - width, GetPosition().y});
            SetSize(core::Float2{width, core::Max(m_minSize.y, s.y + delta.y)});
        }

        void Relayout()
        {
            const core::Float2 s = GetSize();
            m_titleBar->Arrange(Rect{0.0f, 0.0f, s.x, m_titleBarHeight});
            m_contentHost->Arrange(Rect{0.0f, m_titleBarHeight, s.x, core::Max(0.0f, s.y - m_titleBarHeight)});
            const bool rtl = IsRightToLeft();
            m_resizeGrip->Arrange(MirroredIn(Rect{s.x - m_gripSize, s.y - m_gripSize, m_gripSize, m_gripSize},
                                             Rect{0.0f, 0.0f, s.x, s.y}, rtl));
            // The grip's cursor matches its corner: bottom right, or bottom left in right to left.
            m_resizeGrip->SetCursor(rtl ? Cursor::NeswResize : Cursor::NwseResize);
            if (GetChildIndex(m_resizeGrip.Get()) + 1 != ChildCount())
                m_resizeGrip->ToFront(); // keep it above the content
        }

        RefPtr<DragHandle> m_titleBar;
        RefPtr<Label> m_title;
        RefPtr<WindowCloseButton> m_closeButton;
        bool m_closable = true;
        bool m_resizable = true;
        RefPtr<UIWidget> m_contentHost;
        RefPtr<WindowGrip> m_resizeGrip;
        core::Function<void()> m_onClose;
        core::Function<void()> m_onUserArranged;
        bool m_open = false;
        bool m_modal = false;
        u32 m_backHandler = 0; // the `back` command handler while open as a modal
        core::Float2 m_minSize{120.0f, 60.0f};
        f32 m_titleBarHeight = 28.0f;
        f32 m_gripSize = 14.0f;
        Color m_bodyColor{0.16f, 0.17f, 0.21f, 1.0f};
        Color m_titleColor{0.24f, 0.28f, 0.36f, 1.0f};
        Color m_gripColor{0.40f, 0.44f, 0.52f, 1.0f};
    };

    RTTI_DEFINE_OBJECT(DragHandle, "rtti::gui")
    RTTI_DEFINE_OBJECT(WindowCloseButton, "rtti::gui")
    RTTI_DEFINE_OBJECT(WindowGrip, "rtti::gui")
}
