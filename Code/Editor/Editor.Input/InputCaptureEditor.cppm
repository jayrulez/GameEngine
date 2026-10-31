// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Input - :input_capture_editor partition.
//
// InputCaptureEditor: the grid's editor for a binding's input (hint "inputCode": a key, a mouse
// button, a gamepad's button or stick). It is a capture button that shows the current input; when
// clicked, it listens for the next input pressed on any device the action takes. The page does the
// listening (it reads the devices each frame). While it listens the button says so and its focus
// takes every key, so the pressed key runs no shortcut. Escape is an input a game can bind, so it
// does not cancel; clicking again cancels, as does the focus leaving.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.input:input_capture_editor;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;

export namespace editor
{
    namespace gui = foundation::gui;

    /// What the capture editor asks of its page, by the row's path.
    struct InputCaptureServices
    {
        /// The input the row holds, as the page names it ("W", "Gamepad A", "Left Stick").
        Function<String(StringView path)> describe;
        /// Starts listening for the row's input, or stops if it is listening already.
        Function<void(StringView path)> toggle;
        /// Whether the page listens for the row's input now.
        Function<bool(StringView path)> listening;
        /// Stops listening (the focus left the row's button).
        Function<void()> cancel;
    };

    class InputCaptureEditor final : public gui::SingleControlEditor
    {
        RTTI_OBJECT(InputCaptureEditor, gui::SingleControlEditor)
    public:
        static constexpr StringView kListening = u8"Press an input...";

        explicit InputCaptureEditor(const InputCaptureServices& services) : m_services(&services)
        {
            AddClass(u8"input-capture");
            m_button = MakeRef<CaptureButton>(MemoryAllocator(), *this);
            m_button->AddClass(u8"key-capture");
            InputCaptureEditor* self = this;
            m_button->SetOnClick([self]() { self->Toggle(); });
            Hold(*m_button);
        }
        InputCaptureEditor() : InputCaptureEditor(m_none) {} // the form RTTI constructs (with its own empty services)

        /// The editor for every input row `registry` shows; `services` outlives it.
        static void Register(gui::PropertyEditorRegistry& registry, const InputCaptureServices& services)
        {
            const InputCaptureServices* shared = &services;
            registry.RegisterHint(u8"inputCode", [shared](IAllocator& a) -> RefPtr<gui::PropertyEditor> { return MakeRef<InputCaptureEditor>(a, *shared); });
        }

        [[nodiscard]] gui::Button* GetButton() const noexcept { return m_button.Get(); }
        [[nodiscard]] bool IsListening() const { return m_services->listening && m_services->listening(GetPath()); }

    protected:
        void OnBind(const gui::PropertyInfo&, const gui::PropertyValue& value) override
        {
            m_mixed = value.Mixed;
            Show();
        }

    private:
        // Updates the button to the listen state: while listening it says so and takes every key.
        // The editor refreshes itself when a listen starts or stops, rather than the grid doing it,
        // because a refresh of the rows could take its focus and its focus leaving stops the listen.
        void Show()
        {
            const bool listening = IsListening();
            m_button->SetText(listening ? kListening : (m_mixed ? gui::MixedValueText() : Described().AsView()));
            if (listening)
                m_button->AddClass(u8"capturing");
            else
                m_button->RemoveClass(u8"capturing");
            // While it listens its focus takes every key, so no key runs a shortcut.
            m_button->SetCapturesAllKeys(listening);
            if (listening)
                m_button->RequestFocus();
            m_button->SetEnabled(!IsReadOnly());
            m_button->SetTooltip(listening ? StringView(u8"Press the input to bind (click again to stop)") : StringView(u8"Click, then press the input to bind"));
        }

        // The button: its focus leaving stops a listen.
        class CaptureButton final : public gui::Button
        {
        public:
            explicit CaptureButton(InputCaptureEditor& owner) : m_owner(&owner) {}

        protected:
            // The button handles the press itself: otherwise the grid's row would take the focus,
            // and the focus leaving would stop the listen before this click could.
            void OnMouseDown(const gui::MouseEvent& event) override
            {
                if (event.Button == gui::MouseButton::Left)
                    event.Handled = true;
                gui::Button::OnMouseDown(event);
            }
            void OnFocusLost() override
            {
                if (m_owner->IsListening() && m_owner->m_services->cancel)
                {
                    m_owner->m_services->cancel();
                    m_owner->Show();
                }
                gui::Button::OnFocusLost();
            }

        private:
            InputCaptureEditor* m_owner;
        };

        [[nodiscard]] String Described() const { return m_services->describe ? m_services->describe(GetPath()) : String{}; }
        void Toggle()
        {
            if (m_services->toggle)
                m_services->toggle(GetPath());
            Show();
        }

        bool m_mixed = false; // the objects' inputs differ
        InputCaptureServices m_none; // an editor RTTI constructs has no services, so it uses this empty set
        const InputCaptureServices* m_services;
        RefPtr<CaptureButton> m_button;
    };
}
