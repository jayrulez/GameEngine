// Draconic GUI - :button partition
//
// Button: a clickable Label. A lean Draconic-native control modeled on eepp's UIPushButton
// (role only, NOT a line-for-line port: eepp's is ~900 LOC composing a child UIImage icon +
// UITextView with an internal layout). Everything visual is already in place from the base
// layers - the background/skin reacts to the input-driven control state (UINode) and CSS can
// target the default `button` tag - so Button just centers its text and fires a click
// callback (and MouseClick event) when pressed and released on it. Icon + icon/text layout
// are deferred (add via composition when needed).

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:button;

import draconic.foundation; // Function, Move
import :event;        // MouseEvent
import :text;         // TextHAlign / TextVAlign
import :label;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;

export namespace draconic::gui
{
    class Button : public Label
    {
        DRACONIC_OBJECT(Button, Label)
    public:
        Button()
        {
            SetTag(foundation::StringView(u8"button")); // CSS `button { ... }` targets it by default
            SetTextAlignment(TextHAlign::Center, TextVAlign::Middle);
            SetTabFocusable(true);
        }

        // Invoked on a click (press + release on the button).
        void SetOnClick(foundation::Function<void()> callback) { m_onClick = foundation::Move(callback); }
        [[nodiscard]] bool HasOnClick() const noexcept { return static_cast<bool>(m_onClick); }

    protected:
        void OnMouseClick(const MouseEvent& event) override
        {
            (void)event;
            if (m_onClick)
                m_onClick();
        }

        foundation::Function<void()> m_onClick;
    };

    DRACONIC_DEFINE_OBJECT(Button, "draconic::gui")
}
