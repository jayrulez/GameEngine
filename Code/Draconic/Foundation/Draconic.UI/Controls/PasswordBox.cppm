// Draconic UI - :password_box partition
//
// Masked password input. Extends EditText to display mask characters instead of the real text, and
// disables clipboard copy/cut for security. Ported from Sedulous.UI/src/Controls/PasswordBox.bf.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.ui:password_box;

import draconic.foundation; // String, StringView, DecodeUtf8, AppendUtf8
import :edit_text;
import :property;
import :event_args;
import :input_enums; // KeyCode, KeyModifiers, HasFlag

using namespace draconic::foundation;

export namespace draconic::ui
{
    class PasswordBox : public EditText
    {
        DRACONIC_OBJECT(PasswordBox, EditText)
    public:
        /// The character used to mask each real character.
        Property<char32_t> PasswordChar{U'*'};

        PasswordBox()
        {
            Behavior().AllowClipboardCopy = false;
            PasswordChar.SetOwner(this, InvalidationKind::Visual);
        }

        void GetDisplayText(String& outText) const override
        {
            outText.Clear();
            const StringView text = Text();
            const u32 mask = static_cast<u32>(PasswordChar.Value());
            usize i = 0;
            while (i < text.Size())
            {
                (void)DecodeUtf8(text, i);
                AppendUtf8(outText, mask);
            }
        }

        void OnKeyDown(KeyEventArgs& e) override
        {
            // Block copy and cut.
            if (HasFlag(e.Modifiers, KeyModifiers::Ctrl) &&
                (e.Key == KeyCode::C || e.Key == KeyCode::X))
            {
                e.Handled = true;
                return;
            }
            EditText::OnKeyDown(e);
        }
    };

    DRACONIC_DEFINE_OBJECT(PasswordBox, "draconic::ui")
}
