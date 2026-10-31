// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :message_box partition
//
// MessageBox: a modal dialog with a message and a row of buttons. The buttons are either standard
// sets (OK, OK and Cancel, Yes and No) or the caller's own verbs ("Delete", "Create"). It is a
// Window subclass whose content is a word-wrapped message above buttons at the bottom right.
// Pressing a button calls the result callback and closes the dialog. It is shown with
// Window::OpenModal as a modal popup: everything behind it is inert and dimmed by its backdrop.
//
// Each button has a role, like Qt's button roles and GTK's responses, and a class of the same name
// for style sheets. `primary` is the default button, focused when the dialog opens, so Return or
// the pad's South button picks it. `cancel` is what `back` (Escape or the pad's East button) and
// the close button pick. `destructive` marks a button that destroys something; it is styled as
// such and never focused on its own, so Return alone cannot destroy anything. With no primary
// button, the last non-destructive button gets the focus; with no cancel button, `back` picks the
// first. The message wraps and the dialog's height fits it. The dialog is not resizable unless
// asked to be.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:message_box;

import foundation.core;  // RefPtr, MakeRef, Array, Function, Move, Max, Float2
import foundation.fonts; // CachedFont
import :rect;
import :node;
import :label;
import :button;
import :text;
import :ui_widget;
import :window;
import :panel;
import :layout_style;

using namespace foundation::core;
namespace core = foundation::core;
namespace fonts = foundation::fonts;

export namespace foundation::gui
{
    /// What a dialog button does beyond giving its answer. Style sheets style each role by a class
    /// of the same name.
    enum class DialogRole : u8
    {
        Normal,
        Primary,     ///< the default button, focused when the dialog opens
        Cancel,      ///< the answer `back` (Escape) and the close button give
        Destructive, ///< destroys something; never the default on its own
    };
    [[nodiscard]] inline core::StringView DialogRoleName(DialogRole role) noexcept
    {
        switch (role)
        {
        case DialogRole::Primary: return core::StringView(u8"primary");
        case DialogRole::Cancel: return core::StringView(u8"cancel");
        case DialogRole::Destructive: return core::StringView(u8"destructive");
        case DialogRole::Normal:
        default: return core::StringView(u8"normal");
        }
    }
    [[nodiscard]] inline DialogRole DialogRoleNamed(core::StringView name) noexcept
    {
        if (name == core::StringView(u8"primary"))
            return DialogRole::Primary;
        if (name == core::StringView(u8"cancel"))
            return DialogRole::Cancel;
        if (name == core::StringView(u8"destructive"))
            return DialogRole::Destructive;
        return DialogRole::Normal;
    }

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

            // The content is a flow: the wrapping message above a row of buttons at the right.
            {
                StyleWriteScope hostDefaults(*GetContent());
                GetContent()->EditLayoutStyle([](LayoutStyle& st)
                {
                    st.Direction = FlexDirection::Column;
                    st.AlignItems = Align::Stretch;
                    st.RowGap = kGap;
                });
                static_cast<UIWidget*>(GetContent())->SetPadding(Thickness{kPad});
            }
            m_message = core::MakeRef<Label>(MemoryAllocator());
            m_message->AddClass(core::StringView(u8"dialogmessage")); // under a sheet white-space is inherited, so the theme makes it wrap and keep its lines
            {
                StyleWriteScope messageDefaults(*m_message); // defaults: a style sheet overrides them and falls back to them
                m_message->SetWhiteSpace(TextWhiteSpace::PreWrap); // the message wraps and keeps its written line breaks (for a list)
                m_message->SetTextAlignment(TextHAlign::Start, TextVAlign::Top);
                m_message->EditLayoutStyle([](LayoutStyle& st) { st.Grow = 1.0f; });
            }
            GetContent()->AddChild(m_message.Get());
            m_buttonRow = core::MakeRef<Panel>(MemoryAllocator());
            m_buttonRow->SetTag(core::StringView(u8"dialogbuttons"));
            {
                StyleWriteScope rowDefaults(*m_buttonRow);
                m_buttonRow->EditLayoutStyle([](LayoutStyle& st)
                {
                    st.Direction = FlexDirection::Row;
                    st.JustifyContent = Justify::End;
                    st.ColumnGap = kButtonGap;
                    st.Shrink = 0.0f;
                });
            }
            GetContent()->AddChild(m_buttonRow.Get());

            StyleWriteScope defaults(*this); // defaults: a style sheet overrides them and falls back to them
            SetResizable(false);
            SetSize(core::Float2{kWidth, 180.0f});
            LayoutContent();
            // On opening, the default button takes focus, else the cancel button. A destructive
            // button is never focused on its own.
            MessageBox* self = this;
            (void)AddEventListener(EventType::Toggle, [self](const Event& e)
                                   {
                                       if (e.Target != self || !static_cast<const ToggleEvent&>(e).Open)
                                           return;
                                       if (Button* focus = self->GetDefaultButton())
                                           focus->RequestFocus();
                                       else if (Button* cancel = self->ButtonAt(self->CancelIndex()))
                                           cancel->RequestFocus();
                                   });
        }

        // Sets the title, the message and which standard buttons to show.
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
        /// Adds a custom button after the standard ones. `text` is its label (a verb such as
        /// "Delete"). `resultName` is what GetResultName returns, and the change event reports, once
        /// it is pressed. `role` says what else it does: primary, cancel or destructive.
        Button& AddButton(core::StringView text, core::StringView resultName, DialogRole role = DialogRole::Normal)
        {
            Button& button = MakeButton(core::String(resultName), role);
            button.SetText(text);
            LayoutContent();
            return button;
        }
        /// Removes all buttons, before adding the caller's own verbs with AddButton.
        void ClearButtons()
        {
            for (const RefPtr<Button>& b : m_buttons)
                b->RemoveFromParent();
            m_buttons.Clear();
            m_results.Clear();
            m_roles.Clear();
            LayoutContent();
        }
        [[nodiscard]] DialogRole GetButtonRole(usize index) const noexcept
        {
            return index < m_roles.Size() ? m_roles[index] : DialogRole::Normal;
        }
        /// Returns the button focused on opening: the primary one, else the last non-destructive one.
        [[nodiscard]] Button* GetDefaultButton() const
        {
            for (usize i = 0; i < m_roles.Size(); ++i)
                if (m_roles[i] == DialogRole::Primary)
                    return m_buttons[i].Get();
            for (usize i = m_roles.Size(); i-- > 0;)
                if (m_roles[i] != DialogRole::Destructive)
                    return m_buttons[i].Get();
            return nullptr;
        }

        void SetMessageColor(Color color) { m_message->SetTextColor(color); }

        // === Markup ===
        // `<MessageBox id="confirm" title="Delete?" text="..." buttons="ok-cancel"/>` waits hidden
        // in its page until shown (with Show, or a script's show()). Once answered it goes back
        // there, with the answer in `result`, and sends `change`. `buttons` is ok, ok-cancel or
        // yes-no, or the caller's own verbs, each written `Text:result:role` and separated by `|`
        // (`buttons="Cancel:cancel:cancel|Delete:delete:destructive"`). A missing result defaults
        // to the text in lower case, and a missing role to normal.
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
                if (v == core::StringView(u8"ok-cancel") || v == core::StringView(u8"yes-no") || v == core::StringView(u8"ok") || v.IsEmpty())
                    BuildButtons(v == core::StringView(u8"ok-cancel") ? Buttons::OkCancel : v == core::StringView(u8"yes-no") ? Buttons::YesNo : Buttons::Ok);
                else
                    BuildVerbs(v);
                LayoutContent();
                if (!IsOpen())
                    SetDisplayed(false); // a box written in a page stays hidden until shown
                return true;
            }
            return Window::SetMarkupAttribute(name, value);
        }
        /// Opens the box as a modal dialog over the GUI it is in; afterwards it returns to where it
        /// was. Returns false when it is in no GUI, since a box with no parent has nothing to show
        /// over.
        bool Show()
        {
            if (IsOpen())
                return true;
            Node* root = GetRootNode();
            if (root == nullptr || root == this)
                return false;
            // If shown again while its exit animation plays, keep the page it originally came from.
            if (Node* home = GetParent(); home != nullptr && !m_home)
            {
                m_home = core::RefPtr<Node>(home);
                m_homeIndex = home->GetChildIndex(this);
            }
            SetDisplayed(true);
            OpenModal(*root);
            return true;
        }
        /// Returns the last answer, such as "ok", "cancel", "yes" or "no", or empty before any answer.
        [[nodiscard]] core::String GetResultName() const { return m_resultName; }

        [[nodiscard]] core::StringView GetMessage() const { return m_message->GetText(); }
        [[nodiscard]] usize ButtonCount() const noexcept { return m_buttons.Size(); }
        [[nodiscard]] Button* ButtonAt(usize index) const
        {
            return index < m_buttons.Size() ? m_buttons[index].Get() : nullptr;
        }

    public:
        // The close button and `back` (Escape, the pad's East button) give the cancel button's
        // result: the button whose role is cancel (Cancel, No), else the first button.
        [[nodiscard]] bool AnswersBack() const override { return true; }
        void RequestClose() override
        {
            if (m_results.IsEmpty())
                Close();
            else
                Finish(m_results[CancelIndex()].AsView());
        }

    protected:
        // The title (the window's), the message and the buttons use the box's font.
        void OnTextStyleChange() override
        {
            Window::OnTextStyleChange();
            ShareTextStyle(*m_message, false);
            for (const RefPtr<Button>& b : m_buttons)
                ShareTextStyle(*b, false);
            LayoutContent(); // the font changes the message's size
        }

    private:
        // Returns the index of the button `back` answers with: the cancel button, else the first.
        [[nodiscard]] usize CancelIndex() const noexcept
        {
            for (usize i = 0; i < m_roles.Size(); ++i)
                if (m_roles[i] == DialogRole::Cancel)
                    return i;
            return 0;
        }

        void BuildButtons(Buttons buttons)
        {
            ClearButtons();

            switch (buttons)
            {
            case Buttons::Ok:
                AddStandard(Result::Ok);
                break;
            case Buttons::OkCancel:
                AddStandard(Result::Cancel);
                AddStandard(Result::Ok); // the primary button goes rightmost
                break;
            case Buttons::YesNo:
                AddStandard(Result::No);
                AddStandard(Result::Yes); // the primary button goes rightmost
                break;
            }
        }

        // Builds the caller's verb buttons from markup: `Text:result:role|...`.
        void BuildVerbs(core::StringView list)
        {
            ClearButtons();
            usize start = 0;
            while (start <= list.Size())
            {
                usize end = start;
                while (end < list.Size() && list[end] != u8'|')
                    ++end;
                core::StringView fields[3];
                usize field = 0, from = start;
                for (usize i = start; i <= end && field < 3; ++i)
                    if (i == end || list[i] == u8':')
                    {
                        fields[field++] = core::Trim(list.SubStr(from, i - from));
                        from = i + 1;
                    }
                if (!fields[0].IsEmpty())
                {
                    core::String result(fields[1]);
                    if (result.Size() == 0)
                        for (usize i = 0; i < fields[0].Size(); ++i)
                        {
                            const char8_t c = fields[0][i];
                            result.PushBack(c >= u8'A' && c <= u8'Z' ? static_cast<char8_t>(c - u8'A' + u8'a') : c);
                        }
                    (void)AddButton(fields[0], result.AsView(), DialogRoleNamed(fields[2]));
                }
                start = end + 1;
            }
        }

        // Adds a standard button. Its label is translated (gui.ok, ...) where the GUI's tables have
        // the key, otherwise English. OK and Yes are primary; Cancel and No are what `back` picks.
        void AddStandard(Result result)
        {
            static constexpr core::StringView kKeys[] = {u8"gui.ok", u8"gui.cancel", u8"gui.yes", u8"gui.no"};
            static constexpr core::StringView kEnglish[] = {u8"OK", u8"Cancel", u8"Yes", u8"No"};
            static constexpr DialogRole kRoles[] = {DialogRole::Primary, DialogRole::Cancel, DialogRole::Primary, DialogRole::Cancel};
            const usize i = static_cast<usize>(result);
            Button& button = MakeButton(core::String(kNames[i]), kRoles[i]);
            button.SetText(kEnglish[i]);
            (void)button.BindTranslation(core::StringView(u8"text"), kKeys[i], {}, {}, {}, kEnglish[i]);
        }

        static constexpr core::StringView kNames[] = {u8"ok", u8"cancel", u8"yes", u8"no"};

        Button& MakeButton(core::String resultName, DialogRole role)
        {
            auto button = core::MakeRef<Button>(MemoryAllocator());
            ShareTextStyle(*button, false);
            button->AddClass(core::StringView(u8"dialogbutton"));
            if (role != DialogRole::Normal)
                button->AddClass(DialogRoleName(role));
            MessageBox* self = this;
            button->SetOnClick([self, resultName]() { self->Finish(resultName.AsView()); });
            {
                StyleWriteScope buttonDefaults(*button);
                button->EditLayoutStyle([](LayoutStyle& st)
                {
                    st.MinWidth = LayoutLength::Px(kButtonWidth);
                    st.Height = LayoutLength::Px(kButtonHeight);
                });
            }
            m_buttonRow->AddChild(button.Get());
            Button& made = *button;
            m_buttons.PushBack(core::Move(button));
            m_results.PushBack(core::Move(resultName));
            m_roles.PushBack(role);
            return made;
        }

        void Finish(core::StringView resultName)
        {
            m_resultName = core::String(resultName);
            for (usize i = 0; i < 4; ++i) // a standard button also reports its Result
                if (kNames[i] == resultName && m_onResult)
                    m_onResult(static_cast<Result>(i));
            // Closing releases the popup layer's reference, which may be the last one, so keep the
            // box alive until the answer has been reported.
            const core::RefPtr<Node> keep(this);
            Close();
            SendEvent(Event(EventType::Changed, this));
        }
        // The modal was refused as it opened (an unattended run): answer with the cancel button's
        // result, as if it had been pressed.
        void OnModalRefused() override
        {
            if (m_results.IsEmpty())
            {
                Window::OnModalRefused();
                return;
            }
            m_resultName = m_results[CancelIndex()];
            for (usize i = 0; i < 4; ++i) // a standard button also reports its Result
                if (kNames[i] == m_resultName.AsView() && m_onResult)
                    m_onResult(static_cast<Result>(i));
            SendEvent(Event(EventType::Changed, this));
        }
        // A box shown from a page goes back there, hidden, once its exit animation has played.
        void OnWindowGone() override
        {
            if (!m_home)
                return;
            core::RefPtr<Node> home = core::Move(m_home);
            core::RefPtr<Node> keep(this);
            SetDisplayed(false);
            if (GetParent() != home.Get())
                home->AddChildAt(this, core::Min(m_homeIndex, home->ChildCount()));
        }

        // Sets the height to fit the message wrapped at the box's width, plus the buttons; the flow
        // layout places them.
        void LayoutContent()
        {
            if (!m_message || !m_buttonRow)
                return; // may be called during construction
            const f32 inner = core::Max(0.0f, GetSize().x - 2.0f * kPad);
            const f32 text = m_message->Measure(LayoutConstraints{core::Float2{inner, 0.0f}, core::Float2{inner, kUnbounded}, core::Float2{inner, -1.0f}, true, false}).y;
            const f32 height = GetTitleBarHeight() + kPad + text + kGap + kButtonHeight + kPad;
            if (core::Abs(height - GetSize().y) > 0.5f)
            {
                StyleWriteScope own(*this);
                SetSize(core::Float2{GetSize().x, height});
            }
        }

        RefPtr<Label> m_message;
        RefPtr<Panel> m_buttonRow;
        Array<RefPtr<Button>> m_buttons;
        Array<core::String> m_results; // each button's result name, in order
        Array<DialogRole> m_roles;     // each button's role, in order
        core::Function<void(Result)> m_onResult;
        core::String m_resultName;
        core::RefPtr<Node> m_home; // while open, the page it was shown from
        usize m_homeIndex = 0;

        static constexpr f32 kPad = 16.0f;
        static constexpr f32 kGap = 14.0f;
        static constexpr f32 kWidth = 380.0f;
        static constexpr f32 kButtonWidth = 84.0f;
        static constexpr f32 kButtonHeight = 30.0f;
        static constexpr f32 kButtonGap = 10.0f;
    };

}
