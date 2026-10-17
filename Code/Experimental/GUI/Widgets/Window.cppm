// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :window partition
//
// Window: a movable, resizable panel with a title bar and a content area. Modeled on eepp's
// UIWindow (role only). Dragging the title bar moves the window; dragging the bottom-right
// grip resizes it (clamped to a minimum). Pressing anywhere on the title raises the window to
// the front. Movement uses incremental world-space cursor deltas via an internal DragHandle
// that follows the Slider/ScrollBar capture discipline (own m_dragging flag, so a drag that
// leaves the handle keeps tracking). Nested parent transforms are not accounted for (deltas
// are treated as parent-local) - fine for top-level windows under an untransformed root.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:window;

import foundation.core;  // RefPtr, MakeRef, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :rect;
import :event;
import :drawable;
import :rectangle_drawable;
import :node;
import :label;
import :ui_widget;
import :event_dispatcher;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    // A widget that reports incremental world-space cursor deltas while dragged. Internal to
    // Window (title bar + resize grip), but generally reusable.
    class DragHandle : public UIWidget
    {
        RTTI_OBJECT(DragHandle, UIWidget)
    public:
        core::Function<void(core::Float2)> OnDrag; // incremental cursor delta
        core::Function<void()> OnPressed;

    protected:
        // A press moves or resizes the window (E7): a drag from what holds it is not offered.
        [[nodiscard]] bool OwnsPressDrag(core::Float2) const override { return true; }
        void OnMouseDown(const MouseEvent& event) override
        {
            event.Handled = true;
            if (OnPressed)
                OnPressed(); // raise on any button
            if (event.Button != MouseButton::Left)
                return; // but only the left button drags
            m_dragging = true;
            m_last = event.Position;
        }
        void OnMouseMove(const MouseEvent& event) override
        {
            if (!m_dragging)
                return;
            const core::Float2 delta{event.Position.x - m_last.x, event.Position.y - m_last.y};
            m_last = event.Position;
            if (OnDrag)
                OnDrag(delta);
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            (void)event;
            m_dragging = false;
        }
        void OnPointerCancel(const MouseEvent&) override { m_dragging = false; }

    private:
        bool m_dragging = false;
        core::Float2 m_last{0.0f, 0.0f};
    };

    class Window : public UIWidget
    {
        RTTI_OBJECT(Window, UIWidget)
    public:
        Window()
        {
            SetTag(core::StringView(u8"window"));
            SetClipChildren(true);
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), m_bodyColor));

            m_titleBar = core::MakeRef<DragHandle>(MemoryAllocator());
            Window* self = this;
            m_titleBar->OnDrag = [self](core::Float2 d)
            {
                self->SetPosition(
                    core::Float2{self->GetPosition().x + d.x, self->GetPosition().y + d.y});
            };
            m_titleBar->OnPressed = [self]() { self->ToFront(); };
            AddChild(m_titleBar.Get());

            m_title = core::MakeRef<Label>(MemoryAllocator());
            StyleWriteScope defaults(*m_title); // its own values, which a sheet returns to
            m_title->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_title->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            m_title->SetHitTestVisible(false); // clicks fall through to the draggable title bar
            m_titleBar->AddChild(m_title.Get());

            m_contentHost = core::MakeRef<UIWidget>(MemoryAllocator());
            m_contentHost->SetClipChildren(true);
            AddChild(m_contentHost.Get());

            m_resizeGrip = core::MakeRef<DragHandle>(MemoryAllocator());
            m_resizeGrip->OnDrag = [self](core::Float2 d) { self->ResizeBy(d); };
            m_resizeGrip->OnPressed = [self]() { self->ToFront(); };
            m_resizeGrip->SetCursor(Cursor::NwseResize);
            AddChild(m_resizeGrip.Get());
            RefreshPartNodes();
        }

        // The area for window content (add your widgets here).
        [[nodiscard]] Node* GetContent() const noexcept { return m_contentHost.Get(); }
        [[nodiscard]] Node* GetChildHost() override { return m_contentHost.Get(); } // markup's children are its content

        void SetTitle(core::StringView text) { m_title->SetText(text); }
        [[nodiscard]] core::StringView GetTitle() const { return m_title->GetText(); }
        void SetMinSize(core::Float2 size) { m_minSize = size; }
        void SetTitleBarHeight(f32 height)
        {
            m_titleBarHeight = core::Max(1.0f, height);
            Relayout();
        }

        void SetBodyColor(Color color)
        {
            m_bodyColor = color;
            SetBackground(core::MakeRef<RectangleDrawable>(MemoryAllocator(), color));
        }
        void SetTitleColor(Color color)
        {
            m_titleColor = color;
            RefreshPartNodes();
        }

        // Fired when the window is closed via Close().
        void SetOnClose(core::Function<void()> callback) { m_onClose = core::Move(callback); }

        // Show centered in `owner` as a MODAL window (E5): a manual, modal popup of `owner`, so
        // everything below it is inert (pointer, keys, bindings) until Close(), its `::backdrop`
        // part dims the root behind it, Tab stays within, focus moves to its first focusable child
        // and comes back when it closes. Popups opened inside it (a combo box's list) stack above it.
        void OpenModal(Node& owner) { OpenInternal(owner, /*modal*/ true); }

        // Show centered in `root` as a normal (non-blocking) top-level window.
        void Open(Node& root) { OpenInternal(root, /*modal*/ false); }

        // Remove the window (a modal one through the popup stack, which gives focus back), and fire
        // the close callback.
        void Close()
        {
            if (!m_open)
                return;
            EventDispatcher* dispatcher = GetEventDispatcher();
            if (m_modal && dispatcher != nullptr && dispatcher->IsPopupOpen(*this))
            {
                (void)dispatcher->ClosePopup(*this); // its OnClosed finishes
                return;
            }
            RemoveFromParent();
            Closed();
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
        [[nodiscard]] bool IsModal() const noexcept { return m_modal; }

        // Theming parts: window::title (header bar) / ::grip (resize handle).
        void CollectStyleParts(core::Array<core::StringView>& out) const override
        {
            out.PushBack(core::StringView(u8"title"));
            out.PushBack(core::StringView(u8"grip"));
        }
        // Its title bar and grip are nodes of their own: window::title and ::grip give them
        // their boxes, over their own colours.
        void OnPartStyleChange(core::StringView part) override
        {
            if (part == core::StringView(u8"title") || part == core::StringView(u8"grip"))
                RefreshPartNodes();
        }

    protected:
        /// It closed (a dialog written in a page goes back there).
        virtual void OnWindowClosed() {}
        void OnSizeChange() override { Relayout(); }
        // Its title takes its font (a sheet's inheritance gives the same).
        void OnTextStyleChange() override { ShareTextStyle(*m_title, false); }

    private:
        // The title bar's and the grip's boxes: their colours with window::title's and ::grip's over them.
        void RefreshPartNodes()
        {
            const PartStyle& title = GetPartStyle(u8"title");
            const PartStyle& grip = GetPartStyle(u8"grip");
            m_titleBar->SetBox(title.Over(FilledBox(m_titleColor)), false);
            m_resizeGrip->SetBox(grip.Over(FilledBox(m_gripColor)), false);
            StyleWriteScope titleWrite(*m_titleBar);
            m_titleBar->SetAlpha(title.Opacity);
            StyleWriteScope gripWrite(*m_resizeGrip);
            m_resizeGrip->SetAlpha(grip.Opacity);
        }
        void OpenInternal(Node& owner, bool modal)
        {
            if (m_open)
                return;
            EventDispatcher* dispatcher = owner.GetEventDispatcher();
            if (modal && dispatcher != nullptr)
            {
                m_modal = true;
                m_open = true;
                CenterIn(owner.GetRootNode() != nullptr ? *owner.GetRootNode() : owner); // in the root's space
                PopupOptions options;
                options.Kind = PopupKind::Manual;
                options.Modal = true;
                options.TakesFocus = true;
                Window* self = this;
                options.OnClosed = [self](PopupCloseReason) { self->Closed(); };
                dispatcher->OpenPopup(owner, *this, core::Move(options));
                return;
            }
            m_modal = false;
            owner.AddChild(this);
            CenterIn(owner);
            ToFront();
            m_open = true;
        }
        void Closed()
        {
            m_open = false;
            m_modal = false;
            OnWindowClosed();
            if (m_onClose)
                m_onClose();
        }

        void CenterIn(Node& root)
        {
            const core::Float2 r = root.GetSize();
            const core::Float2 s = GetSize();
            SetPosition(core::Float2{(r.x - s.x) * 0.5f, (r.y - s.y) * 0.5f});
        }

        void ResizeBy(core::Float2 delta)
        {
            const core::Float2 s = GetSize();
            SetSize(core::Float2{core::Max(m_minSize.x, s.x + delta.x),
                                 core::Max(m_minSize.y, s.y + delta.y)});
        }

        void Relayout()
        {
            const core::Float2 s = GetSize();
            m_titleBar->SetPosition(core::Float2{0.0f, 0.0f});
            m_titleBar->SetSize(core::Float2{s.x, m_titleBarHeight});
            m_title->SetSize(core::Float2{s.x, m_titleBarHeight});

            m_contentHost->SetPosition(core::Float2{0.0f, m_titleBarHeight});
            m_contentHost->SetSize(core::Float2{s.x, core::Max(0.0f, s.y - m_titleBarHeight)});

            m_resizeGrip->SetSize(core::Float2{m_gripSize, m_gripSize});
            m_resizeGrip->SetPosition(core::Float2{s.x - m_gripSize, s.y - m_gripSize});
        }

        RefPtr<DragHandle> m_titleBar;
        RefPtr<Label> m_title;
        RefPtr<UIWidget> m_contentHost;
        RefPtr<DragHandle> m_resizeGrip;
        core::Function<void()> m_onClose;
        bool m_open = false;
        bool m_modal = false;
        core::Float2 m_minSize{120.0f, 60.0f};
        f32 m_titleBarHeight = 28.0f;
        f32 m_gripSize = 14.0f;
        Color m_bodyColor{0.16f, 0.17f, 0.21f, 1.0f};
        Color m_titleColor{0.24f, 0.28f, 0.36f, 1.0f};
        Color m_gripColor{0.40f, 0.44f, 0.52f, 1.0f};
    };

    RTTI_DEFINE_OBJECT(DragHandle, "rtti::gui")
}
