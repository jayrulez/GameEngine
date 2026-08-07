// Draconic GUI - :message_box partition
//
// MessageBox: a modal dialog with a message and a row of standard buttons (OK / OK-Cancel /
// Yes-No). Modeled on eepp's UIMessageBox (role only) - a Window subclass whose content is a
// word-wrapped message plus bottom-right buttons. Pressing a button fires the result callback
// and closes the dialog. Shown via Window::OpenModal, so the dispatcher confines input to it
// and a dim scrim covers the background.

module;
#include "Draconic.Foundation/Prelude.h"
#include "Draconic.Foundation/Reflection/Reflect.h"

export module draconic.gui:message_box;

import draconic.foundation;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import draconic.fonts; // CachedFont
import :rect;
import :node;
import :label;
import :button;
import :text;
import :ui_widget;
import :window;

using namespace draconic::foundation;
namespace foundation = draconic::foundation;
namespace fonts = draconic::fonts;

export namespace draconic::gui
{
    class MessageBox : public Window
    {
        DRACONIC_OBJECT(MessageBox, Window)
    public:
        enum class Buttons
        {
            Ok,
            OkCancel,
            YesNo
        };
        enum class Result
        {
            Ok,
            Cancel,
            Yes,
            No
        };

        MessageBox()
        {
            SetTag(foundation::StringView(u8"messagebox"));

            // Create the message before SetSize, since SetSize -> OnSizeChange -> LayoutContent
            // reads m_message.
            m_message = foundation::MakeRef<Label>(foundation::DefaultAllocator());
            m_message->SetWordWrap(true);
            m_message->SetTextAlignment(TextHAlign::Left, TextVAlign::Top);
            GetContent()->AddChild(m_message.Get());

            SetSize(foundation::Float2{360.0f, 180.0f});
            LayoutContent();
        }

        // Set the title, message, and which buttons to show.
        void Configure(foundation::StringView title, foundation::StringView message, Buttons buttons)
        {
            SetTitle(title);
            m_message->SetText(message);
            BuildButtons(buttons);
            LayoutContent();
        }

        void SetOnResult(foundation::Function<void(Result)> callback)
        {
            m_onResult = foundation::Move(callback);
        }

        // Font reaches the title (base), the message, and the buttons.
        void SetFont(fonts::CachedFont* font)
        {
            Window::SetFont(font);
            m_font = font;
            m_message->SetFont(font);
            for (const RefPtr<Button>& b : m_buttons)
                b->SetFont(font);
        }
        void SetMessageColor(Color color) { m_message->SetTextColor(color); }

        [[nodiscard]] foundation::StringView GetMessage() const { return m_message->GetText(); }
        [[nodiscard]] usize ButtonCount() const noexcept { return m_buttons.Size(); }
        [[nodiscard]] Button* ButtonAt(usize index) const
        {
            return index < m_buttons.Size() ? m_buttons[index].Get() : nullptr;
        }

    protected:
        void OnSizeChange() override
        {
            Window::OnSizeChange(); // relayout title bar / content host / grip
            LayoutContent();
        }

    private:
        void BuildButtons(Buttons buttons)
        {
            for (const RefPtr<Button>& b : m_buttons)
                b->RemoveFromParent();
            m_buttons.Clear();

            switch (buttons)
            {
            case Buttons::Ok:
                AddButton(foundation::StringView(u8"OK"), Result::Ok);
                break;
            case Buttons::OkCancel:
                AddButton(foundation::StringView(u8"Cancel"), Result::Cancel);
                AddButton(foundation::StringView(u8"OK"), Result::Ok); // primary rightmost
                break;
            case Buttons::YesNo:
                AddButton(foundation::StringView(u8"No"), Result::No);
                AddButton(foundation::StringView(u8"Yes"), Result::Yes); // primary rightmost
                break;
            }
        }

        void AddButton(foundation::StringView text, Result result)
        {
            auto button = foundation::MakeRef<Button>(foundation::DefaultAllocator());
            button->SetText(text);
            button->SetFont(m_font);
            button->AddClass(foundation::StringView(u8"dialogbutton"));
            MessageBox* self = this;
            button->SetOnClick([self, result]() { self->Finish(result); });
            GetContent()->AddChild(button.Get());
            m_buttons.PushBack(foundation::Move(button));
        }

        void Finish(Result result)
        {
            if (m_onResult)
                m_onResult(result);
            Close();
        }

        void LayoutContent()
        {
            Node* content = GetContent();
            if (content == nullptr || !m_message)
                return; // may run mid-construction
            const foundation::Float2 cs = content->GetSize();

            const f32 buttonAreaTop = foundation::Max(0.0f, cs.y - kButtonHeight - kPad);
            m_message->SetPosition(foundation::Float2{kPad, kPad});
            m_message->SetSize(foundation::Float2{foundation::Max(0.0f, cs.x - 2.0f * kPad),
                                            foundation::Max(0.0f, buttonAreaTop - kPad)});

            // Right-align the button group at the bottom.
            const usize n = m_buttons.Size();
            if (n == 0)
                return;
            const f32 totalWidth =
                static_cast<f32>(n) * kButtonWidth + static_cast<f32>(n - 1) * kButtonGap;
            const f32 startX = foundation::Max(kPad, cs.x - kPad - totalWidth);
            for (usize i = 0; i < n; ++i)
            {
                m_buttons[i]->SetPosition(foundation::Float2{
                    startX + static_cast<f32>(i) * (kButtonWidth + kButtonGap), buttonAreaTop});
                m_buttons[i]->SetSize(foundation::Float2{kButtonWidth, kButtonHeight});
            }
        }

        RefPtr<Label> m_message;
        Array<RefPtr<Button>> m_buttons;
        fonts::CachedFont* m_font = nullptr;
        foundation::Function<void(Result)> m_onResult;

        static constexpr f32 kPad = 16.0f;
        static constexpr f32 kButtonWidth = 84.0f;
        static constexpr f32 kButtonHeight = 30.0f;
        static constexpr f32 kButtonGap = 10.0f;
    };

    DRACONIC_DEFINE_OBJECT(MessageBox, "draconic::gui")
}
