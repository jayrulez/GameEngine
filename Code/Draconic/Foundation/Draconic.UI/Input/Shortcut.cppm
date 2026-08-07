// Draconic UI - :shortcut partition
//
// A keyboard shortcut binding: key + modifiers -> action. Ported from Sedulous.UI/src/Input/Shortcut.bf.
// Object (RefCounted) so ShortcutManager can own them via RefPtr and hand back stable pointers.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:shortcut;

import draconic.foundation; // Function, String, Object
import :input_enums;  // KeyCode, KeyModifiers

using namespace draconic::foundation;

export namespace draconic::ui
{
    class View; // Scope back-pointer (non-owning); defined in :view.

    class Shortcut : public Object
    {
        DRACONIC_OBJECT(Shortcut, Object)
    public:
        KeyCode Key = KeyCode::Unknown;
        KeyModifiers Modifiers = KeyModifiers::None;
        String DisplayText{};    ///< Menu text (empty = auto).
        Function<void()> Action; ///< Executed when the shortcut fires.
        View* Scope = nullptr;   ///< Null = global (fires regardless of focus).
        bool IsEnabled = true;

        Shortcut() = default;
        Shortcut(KeyCode key, KeyModifiers modifiers, Function<void()> action,
                 View* scope = nullptr)
            : Key(key), Modifiers(modifiers), Action(Move(action)), Scope(scope)
        {
        }

        /// Whether this shortcut matches the given key event (Left/Right modifiers normalized).
        [[nodiscard]] bool Matches(KeyCode key, KeyModifiers modifiers) const
        {
            if (key != Key)
            {
                return false;
            }
            return Normalize(Modifiers) == Normalize(modifiers);
        }

    private:
        /// Collapse Left/Right modifier variants into combined flags; strip lock keys.
        [[nodiscard]] static KeyModifiers Normalize(KeyModifiers m)
        {
            KeyModifiers r = m;
            if (HasFlag(r, KeyModifiers::LeftShift) || HasFlag(r, KeyModifiers::RightShift))
            {
                r |= KeyModifiers::Shift;
            }
            if (HasFlag(r, KeyModifiers::LeftCtrl) || HasFlag(r, KeyModifiers::RightCtrl))
            {
                r |= KeyModifiers::Ctrl;
            }
            if (HasFlag(r, KeyModifiers::LeftAlt) || HasFlag(r, KeyModifiers::RightAlt))
            {
                r |= KeyModifiers::Alt;
            }
            if (HasFlag(r, KeyModifiers::LeftGui) || HasFlag(r, KeyModifiers::RightGui))
            {
                r |= KeyModifiers::Gui;
            }
            return r & (KeyModifiers::Ctrl | KeyModifiers::Shift | KeyModifiers::Alt |
                        KeyModifiers::Gui);
        }
    };

    DRACONIC_DEFINE_OBJECT(Shortcut, "draconic::ui")
}
