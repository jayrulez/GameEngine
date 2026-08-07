// Draconic GUI - :window partition
//
// Window: a movable, resizable panel with a title bar and a content area. Modeled on eepp's
// UIWindow (role only). Dragging the title bar moves the window; dragging the bottom-right
// grip resizes it (clamped to a minimum). Pressing anywhere on the title raises the window to
// the front. Movement uses incremental world-space cursor deltas via an internal DragHandle
// that follows the Slider/ScrollBar capture discipline (own m_dragging flag, so a drag that
// leaves the handle keeps tracking). Nested parent transforms are not accounted for (deltas
// are treated as parent-local) - fine for top-level windows under an untransformed root.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:window;

import draconic.foundation;  // RefPtr, MakeRef, Function, Move, Max, Float2
import draconic.fonts; // CachedFont
import :rect;
import :event;
import :drawable;
import :rectangle_drawable;
import :node;
import :label;
import :ui_widget;
import :event_dispatcher;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    // A widget that reports incremental world-space cursor deltas while dragged. Internal to
    // Window (title bar + resize grip), but generally reusable.
    class DragHandle : public UIWidget
    {
        DRACONIC_OBJECT(DragHandle, UIWidget)
    public:
        foundation::Function<void(foundation::Float2)> OnDrag; // incremental cursor delta
        foundation::Function<void()> OnPressed;

    protected:
        void OnMouseDown(const MouseEvent& event) override
        {
            UINode::OnMouseDown(event);
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
            const foundation::Float2 delta{event.Position.x - m_last.x, event.Position.y - m_last.y};
            m_last = event.Position;
            if (OnDrag)
                OnDrag(delta);
        }
        void OnMouseUp(const MouseEvent& event) override
        {
            m_dragging = false;
            UINode::OnMouseUp(event);
        }

    private:
        bool m_dragging = false;
        foundation::Float2 m_last{0.0f, 0.0f};
    };

    class Window : public UIWidget
    {
        DRACONIC_OBJECT(Window, UIWidget)
    public:
        Window()
        {
            SetTag(foundation::StringView(u8"window"));
            SetClipChildren(true);
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_bodyColor));

            m_titleBar = foundation::MakeRef<DragHandle>(foundation::DefaultAllocator());
            m_titleBar->SetBackground(
                foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_titleColor));
            Window* self = this;
            m_titleBar->OnDrag = [self](foundation::Float2 d)
            {
                self->SetPosition(
                    foundation::Float2{self->GetPosition().x + d.x, self->GetPosition().y + d.y});
            };
            m_titleBar->OnPressed = [self]() { self->ToFront(); };
            AddChild(m_titleBar.Get());

            m_title = foundation::MakeRef<Label>(foundation::DefaultAllocator());
            m_title->SetTextAlignment(TextHAlign::Left, TextVAlign::Middle);
            m_title->SetPadding(Thickness{8.0f, 0.0f, 8.0f, 0.0f});
            m_title->SetHitTestVisible(false); // clicks fall through to the draggable title bar
            m_titleBar->AddChild(m_title.Get());

            m_contentHost = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
            m_contentHost->SetClipChildren(true);
            AddChild(m_contentHost.Get());

            m_resizeGrip = foundation::MakeRef<DragHandle>(foundation::DefaultAllocator());
            m_resizeGrip->SetBackground(
                foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_gripColor));
            m_resizeGrip->OnDrag = [self](foundation::Float2 d) { self->ResizeBy(d); };
            m_resizeGrip->OnPressed = [self]() { self->ToFront(); };
            AddChild(m_resizeGrip.Get());
        }

        // The area for window content (add your widgets here).
        [[nodiscard]] Node* GetContent() const noexcept { return m_contentHost.Get(); }

        void SetTitle(foundation::StringView text) { m_title->SetText(text); }
        void SetFont(fonts::CachedFont* font) { m_title->SetFont(font); }
        void SetMinSize(foundation::Float2 size) { m_minSize = size; }
        void SetTitleBarHeight(f32 height)
        {
            m_titleBarHeight = foundation::Max(1.0f, height);
            Relayout();
        }

        void SetBodyColor(Color color)
        {
            m_bodyColor = color;
            SetBackground(foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), color));
        }
        void SetTitleColor(Color color)
        {
            m_titleColor = color;
            m_titleBar->SetBackground(
                foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), color));
        }

        // Fired when the window is closed via Close().
        void SetOnClose(foundation::Function<void()> callback) { m_onClose = foundation::Move(callback); }

        // Show centered in `root` as a MODAL window: a dim scrim covers everything behind it and
        // the dispatcher confines input to this window until Close() (clicks outside are ignored,
        // Tab stays within). Focus moves to the first focusable child.
        void OpenModal(Node& root) { OpenInternal(root, /*modal*/ true); }

        // Show centered in `root` as a normal (non-blocking) top-level window.
        void Open(Node& root) { OpenInternal(root, /*modal*/ false); }

        // Remove the window (and its scrim), release any modal, and fire the close callback.
        void Close()
        {
            if (!m_open)
                return;
            m_open = false;
            if (m_modal)
                if (EventDispatcher* dispatcher = GetEventDispatcher())
                    if (dispatcher->GetModalRoot() == this)
                        dispatcher->SetModalRoot(nullptr);
            if (m_scrim)
            {
                m_scrim->RemoveFromParent();
                m_scrim.Reset();
            }
            RemoveFromParent();
            m_modal = false;
            if (m_onClose)
                m_onClose();
        }

        [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
        [[nodiscard]] bool IsModal() const noexcept { return m_modal; }
        void SetScrimColor(Color color) { m_scrimColor = color; }

        // Theming parts: window::title (header bar) / ::grip (resize handle).
        void CollectStyleParts(foundation::Array<foundation::StringView>& out) const override
        {
            out.PushBack(foundation::StringView(u8"title"));
            out.PushBack(foundation::StringView(u8"grip"));
        }
        void SetThemePartColor(foundation::StringView part, Color color) override
        {
            if (part == foundation::StringView(u8"title"))
                m_titleBar->SetBackground(
                    foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), color));
            else if (part == foundation::StringView(u8"grip"))
                m_resizeGrip->SetBackground(
                    foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), color));
        }

    protected:
        void OnSizeChange() override { Relayout(); }

    private:
        void OpenInternal(Node& root, bool modal)
        {
            if (m_open)
                return;
            m_modal = modal;
            if (modal)
            {
                m_scrim = foundation::MakeRef<UIWidget>(foundation::DefaultAllocator());
                m_scrim->SetSize(root.GetSize());
                m_scrim->SetBackground(
                    foundation::MakeRef<RectangleDrawable>(foundation::DefaultAllocator(), m_scrimColor));
                root.AddChild(m_scrim.Get()); // below the window (added first)
            }
            root.AddChild(this); // on top of the scrim
            CenterIn(root);
            ToFront();
            m_open = true;
            if (modal)
                if (EventDispatcher* dispatcher = GetEventDispatcher())
                {
                    dispatcher->SetModalRoot(this);
                    dispatcher->FocusNext(); // focus the first focusable within the modal
                }
        }

        void CenterIn(Node& root)
        {
            const foundation::Float2 r = root.GetSize();
            const foundation::Float2 s = GetSize();
            SetPosition(foundation::Float2{(r.x - s.x) * 0.5f, (r.y - s.y) * 0.5f});
        }

        void ResizeBy(foundation::Float2 delta)
        {
            const foundation::Float2 s = GetSize();
            SetSize(foundation::Float2{foundation::Max(m_minSize.x, s.x + delta.x),
                                 foundation::Max(m_minSize.y, s.y + delta.y)});
        }

        void Relayout()
        {
            const foundation::Float2 s = GetSize();
            m_titleBar->SetPosition(foundation::Float2{0.0f, 0.0f});
            m_titleBar->SetSize(foundation::Float2{s.x, m_titleBarHeight});
            m_title->SetSize(foundation::Float2{s.x, m_titleBarHeight});

            m_contentHost->SetPosition(foundation::Float2{0.0f, m_titleBarHeight});
            m_contentHost->SetSize(foundation::Float2{s.x, foundation::Max(0.0f, s.y - m_titleBarHeight)});

            m_resizeGrip->SetSize(foundation::Float2{m_gripSize, m_gripSize});
            m_resizeGrip->SetPosition(foundation::Float2{s.x - m_gripSize, s.y - m_gripSize});
        }

        RefPtr<DragHandle> m_titleBar;
        RefPtr<Label> m_title;
        RefPtr<UIWidget> m_contentHost;
        RefPtr<DragHandle> m_resizeGrip;
        RefPtr<UIWidget> m_scrim; // dim overlay behind a modal (owned while open)
        foundation::Function<void()> m_onClose;
        bool m_open = false;
        bool m_modal = false;
        Color m_scrimColor{0.0f, 0.0f, 0.0f, 0.45f};
        foundation::Float2 m_minSize{120.0f, 60.0f};
        f32 m_titleBarHeight = 28.0f;
        f32 m_gripSize = 14.0f;
        Color m_bodyColor{0.16f, 0.17f, 0.21f, 1.0f};
        Color m_titleColor{0.24f, 0.28f, 0.36f, 1.0f};
        Color m_gripColor{0.40f, 0.44f, 0.52f, 1.0f};
    };

    DRACONIC_DEFINE_OBJECT(DragHandle, "draconic::gui")
    DRACONIC_DEFINE_OBJECT(Window, "draconic::gui")
}
