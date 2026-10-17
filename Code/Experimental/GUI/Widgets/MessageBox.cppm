// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :message_box partition
//
// MessageBox: a modal dialog with a message and a row of standard buttons (OK / OK-Cancel /
// Yes-No). Modeled on eepp's UIMessageBox (role only) - a Window subclass whose content is a
// word-wrapped message plus bottom-right buttons. Pressing a button fires the result callback
// and closes the dialog. Shown via Window::OpenModal, a modal popup (E5): what is behind it is
// inert and its backdrop dims it. Its default button (the rightmost, OK or Yes) has focus when it
// opens, so Return or a pad's South picks it; Escape (or a pad's East) picks the cancel one
// (Cancel, No, or OK when it is the only one).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module experimental.gui:message_box;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :rect;
import :node;
import :label;
import :button;
import :text;
import :ui_widget;
import :window;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace experimental::gui
{
    class MessageBox : public Window
    {
        RTTI_OBJECT(MessageBox, Window)
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
            SetTag(core::StringView(u8"messagebox"));

            // Create the message before SetSize, since SetSize -> OnSizeChange -> LayoutContent
            // reads m_message.
            m_message = core::MakeRef<Label>(MemoryAllocator());
            StyleWriteScope messageDefaults(*m_message); // its own values, which a sheet returns to
            m_message->SetWhiteSpace(TextWhiteSpace::Normal); // a message wraps
            m_message->SetTextAlignment(TextHAlign::Left, TextVAlign::Top);
            GetContent()->AddChild(m_message.Get());

            StyleWriteScope defaults(*this); // its own values, which a sheet returns to
            SetSize(core::Float2{360.0f, 180.0f});
            LayoutContent();
            // Open, the default button takes focus.
            MessageBox* self = this;
            (void)AddEventListener(EventType::Toggle, [self](const Event& e)
                                   {
                                       if (e.Target == self && static_cast<const ToggleEvent&>(e).Open && !self->m_buttons.IsEmpty())
                                           self->m_buttons.Back()->RequestFocus();
                                   });
        }

        // Set the title, message, and which buttons to show.
        void Configure(core::StringView title, core::StringView message, Buttons buttons)
        {
            SetTitle(title);
            m_message->SetText(message);
            BuildButtons(buttons);
            LayoutContent();
        }

        void SetOnResult(core::Function<void(Result)> callback)
        {
            m_onResult = core::Move(callback);
        }

        void SetMessageColor(Color color) { m_message->SetTextColor(color); }

        // === Markup ===
        // `<MessageBox id="confirm" title="Delete?" text="..." buttons="ok-cancel"/>` waits in its
        // page, unseen, until shown (Show, a script's show()); answered, it goes back there, its
        // `result` the answer, reported as `change`.
        bool SetMarkupAttribute(core::StringView name, core::StringView value) override
        {
            if (name == core::StringView(u8"text"))
            {
                m_message->SetText(value);
                LayoutContent();
                return true;
            }
            if (name == core::StringView(u8"buttons"))
            {
                const core::StringView v = core::Trim(value);
                BuildButtons(v == core::StringView(u8"ok-cancel") ? Buttons::OkCancel : v == core::StringView(u8"yes-no") ? Buttons::YesNo : Buttons::Ok);
                LayoutContent();
                if (!IsOpen())
                    SetDisplayed(false); // written in a page: unseen until shown
                return true;
            }
            return Window::SetMarkupAttribute(name, value);
        }
        /// Shows it as a modal dialog over the GUI it is in (it comes back to where it was).
        void Show()
        {
            if (IsOpen())
                return;
            Node* root = GetRootNode();
            if (Node* home = GetParent())
            {
                m_home = core::RefPtr<Node>(home);
                m_homeIndex = home->GetChildIndex(this);
            }
            SetDisplayed(true);
            OpenModal(*root);
        }
        /// The last answer: "ok", "cancel", "yes" or "no" (empty: none yet).
        [[nodiscard]] core::String GetResultName() const { return m_resultName; }

        [[nodiscard]] core::StringView GetMessage() const { return m_message->GetText(); }
        [[nodiscard]] usize ButtonCount() const noexcept { return m_buttons.Size(); }
        [[nodiscard]] Button* ButtonAt(usize index) const
        {
            return index < m_buttons.Size() ? m_buttons[index].Get() : nullptr;
        }

    protected:
        // Escape (a pad's East, unbound) picks the cancel button's result.
        void OnKeyDown(const KeyEvent& event) override
        {
            if (event.KeyCode != static_cast<u32>(KeyCode::Escape) || event.Modifiers != 0 || m_results.IsEmpty())
                return;
            event.Handled = true;
            Finish(m_results[0]); // the leftmost: Cancel or No, or the only one
        }

        void OnSizeChange() override
        {
            Window::OnSizeChange(); // relayout title bar / content host / grip
            LayoutContent();
        }

    protected:
        // Its title (the window's), message and buttons take its font.
        void OnTextStyleChange() override
        {
            Window::OnTextStyleChange();
            ShareTextStyle(*m_message, false);
            for (const RefPtr<Button>& b : m_buttons)
                ShareTextStyle(*b, false);
        }

    private:
        void BuildButtons(Buttons buttons)
        {
            for (const RefPtr<Button>& b : m_buttons)
                b->RemoveFromParent();
            m_buttons.Clear();
            m_results.Clear();

            switch (buttons)
            {
            case Buttons::Ok:
                AddButton(core::StringView(u8"OK"), Result::Ok);
                break;
            case Buttons::OkCancel:
                AddButton(core::StringView(u8"Cancel"), Result::Cancel);
                AddButton(core::StringView(u8"OK"), Result::Ok); // primary rightmost
                break;
            case Buttons::YesNo:
                AddButton(core::StringView(u8"No"), Result::No);
                AddButton(core::StringView(u8"Yes"), Result::Yes); // primary rightmost
                break;
            }
        }

        void AddButton(core::StringView text, Result result)
        {
            auto button = core::MakeRef<Button>(MemoryAllocator());
            button->SetText(text);
            ShareTextStyle(*button, false);
            button->AddClass(core::StringView(u8"dialogbutton"));
            MessageBox* self = this;
            button->SetOnClick([self, result]() { self->Finish(result); });
            GetContent()->AddChild(button.Get());
            m_buttons.PushBack(core::Move(button));
            m_results.PushBack(result);
        }

        void Finish(Result result)
        {
            static constexpr core::StringView kNames[] = {u8"ok", u8"cancel", u8"yes", u8"no"};
            m_resultName = core::String(kNames[static_cast<usize>(result)]);
            if (m_onResult)
                m_onResult(result);
            Close();
            SendEvent(Event(EventType::Changed, this));
        }
        // Shown from a page: back there, unseen.
        void OnWindowClosed() override
        {
            if (!m_home)
                return;
            core::RefPtr<Node> home = core::Move(m_home);
            core::RefPtr<Node> keep(this);
            SetDisplayed(false);
            if (GetParent() != home.Get())
                home->AddChildAt(this, core::Min(m_homeIndex, home->ChildCount()));
        }

        void LayoutContent()
        {
            Node* content = GetContent();
            if (content == nullptr || !m_message)
                return; // may run mid-construction
            const core::Float2 cs = content->GetSize();

            const f32 buttonAreaTop = core::Max(0.0f, cs.y - kButtonHeight - kPad);
            m_message->SetPosition(core::Float2{kPad, kPad});
            m_message->SetSize(core::Float2{core::Max(0.0f, cs.x - 2.0f * kPad),
                                            core::Max(0.0f, buttonAreaTop - kPad)});

            // Right-align the button group at the bottom.
            const usize n = m_buttons.Size();
            if (n == 0)
                return;
            const f32 totalWidth =
                static_cast<f32>(n) * kButtonWidth + static_cast<f32>(n - 1) * kButtonGap;
            const f32 startX = core::Max(kPad, cs.x - kPad - totalWidth);
            for (usize i = 0; i < n; ++i)
            {
                m_buttons[i]->SetPosition(core::Float2{
                    startX + static_cast<f32>(i) * (kButtonWidth + kButtonGap), buttonAreaTop});
                m_buttons[i]->SetSize(core::Float2{kButtonWidth, kButtonHeight});
            }
        }

        RefPtr<Label> m_message;
        Array<RefPtr<Button>> m_buttons;
        Array<Result> m_results; // each button's, in order
        core::Function<void(Result)> m_onResult;
        core::String m_resultName;
        core::RefPtr<Node> m_home; // the page it was shown from, while it is open
        usize m_homeIndex = 0;

        static constexpr f32 kPad = 16.0f;
        static constexpr f32 kButtonWidth = 84.0f;
        static constexpr f32 kButtonHeight = 30.0f;
        static constexpr f32 kButtonGap = 10.0f;
    };

}
