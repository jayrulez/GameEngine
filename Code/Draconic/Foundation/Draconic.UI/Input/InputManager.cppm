// Draconic UI - :input_manager partition
//
// Routes input events to views (hover/pressed/capture tracked by ViewId), with three-phase
// capture->target->bubble dispatch and pooled event args. Ported from Sedulous.UI/src/Input/InputManager.bf.
// All View-touching bodies live in the module impl unit. DEFERRED (guarded/omitted until their
// subsystems land): drag-drop priority + IDragSource init, tooltip hide/hover, popup click-outside.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.ui:input_manager;

import draconic.foundation;
import :view_id;
import :input_enums; // MouseButton, KeyCode, KeyModifiers
import :event_args;
import :enums; // CursorType

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View;
    class UIContext;

    class InputManager
    {
    public:
        explicit InputManager(UIContext* context) : m_context(context) {}

        /// Maximum time (s) / distance (px) between clicks for a double-click.
        f32 DoubleClickTime = 0.5f;
        f32 DoubleClickDistance = 4.0f;

        // === Mouse (coordinates in physical pixels) ===
        bool ProcessMouseMove(f32 physicalX, f32 physicalY);
        bool ProcessMouseDown(MouseButton button, f32 physicalX, f32 physicalY, f32 totalTime);
        bool ProcessMouseUp(MouseButton button, f32 physicalX, f32 physicalY);
        bool ProcessMouseWheel(f32 physicalX, f32 physicalY, f32 deltaX, f32 deltaY,
                               KeyModifiers modifiers = KeyModifiers::None);

        // === Keyboard ===
        /// The modifier state from the most recent key event - stamped into mouse events so
        /// Ctrl/Shift+click behaviors (multi-select) work.
        [[nodiscard]] KeyModifiers CurrentModifiers() const noexcept { return m_currentModifiers; }

        bool ProcessKeyDown(KeyCode key, KeyModifiers modifiers, bool isRepeat,
                            f32 timestamp = 0.0f);
        bool ProcessKeyUp(KeyCode key, KeyModifiers modifiers, f32 timestamp = 0.0f);
        bool ProcessTextInput(char32_t character);

        // === Deletion safety ===
        void OnViewDeleted(View* view);

        // === Queries ===
        [[nodiscard]] ViewId HoveredId() const noexcept { return m_hoveredId; }
        [[nodiscard]] ViewId PressedId() const noexcept { return m_pressedId; }
        [[nodiscard]] f32 MouseX() const noexcept { return m_mouseX; }
        [[nodiscard]] f32 MouseY() const noexcept { return m_mouseY; }
        [[nodiscard]] CursorType CurrentCursor() const noexcept { return m_currentCursor; }

    private:
        // Internal (impl unit).
        bool SearchAccelerator(View* view, KeyCode key, KeyModifiers modifiers);
        void UpdateHover(f32 x, f32 y);
        void FocusNearestFocusable(View* view);
        i32 BuildAncestorChain(View* target);
        void DispatchMouseDown(View* target, MouseEventArgs& args);
        void DispatchMouseUp(View* target, MouseEventArgs& args);
        void DispatchKeyDown(View* target, KeyEventArgs& args);
        void DispatchKeyUp(View* target, KeyEventArgs& args);
        void DispatchMouseWheel(View* target, MouseWheelEventArgs& args);
        void DispatchTextInput(View* target, TextInputEventArgs& args);

        UIContext* m_context = nullptr;

        ViewId m_hoveredId{};
        ViewId m_pressedId{};
        MouseButton m_pressedButton = MouseButton::Left;

        f32 m_mouseX = 0.0f;
        f32 m_mouseY = 0.0f;

        f32 m_lastClickTime = 0.0f;
        f32 m_lastClickX = 0.0f;
        f32 m_lastClickY = 0.0f;
        i32 m_clickCount = 0;

        CursorType m_currentCursor = CursorType::Default;

        // Pooled event args (reused each event).
        MouseEventArgs m_mouseArgs;
        KeyModifiers m_currentModifiers = KeyModifiers::None;
        MouseWheelEventArgs m_wheelArgs;
        KeyEventArgs m_keyArgs;
        TextInputEventArgs m_textArgs;

        static constexpr usize kMaxAncestors = 64;
        View* m_ancestorChain[kMaxAncestors] = {};
    };
}
