// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :shortcut_capture partition.
//
// ShortcutCaptureButton: a button that shows a chord and, once clicked, takes the next key the
// user presses as the new one, with the modifiers held. While it waits its focus captures every
// key (Node::SetCapturesAllKeys), so no binding runs the chord instead. Escape cancels the
// capture, Backspace or Delete chooses "no shortcut", a modifier alone is not a chord, and losing
// focus cancels. Preferences > Shortcuts puts one in every row.
module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.app:shortcut_capture;

import foundation.core;
import foundation.gui;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class ShortcutCaptureButton final : public gui::Button
    {
        RTTI_OBJECT(ShortcutCaptureButton, gui::Button)
    public:
        ShortcutCaptureButton()
        {
            AddClass(u8"shortcut-capture");
            ShowChord();
            ShortcutCaptureButton* self = this;
            SetOnClick([self]() { self->BeginCapture(); });
        }

        /// Called with the chord the user chose (unset: no shortcut); the button shows it.
        void SetOnChordChosen(Function<void(EditorShortcut)> chosen) { m_onChosen = foundation::core::Move(chosen); }

        [[nodiscard]] EditorShortcut Chord() const noexcept { return m_chord; }
        void SetChord(EditorShortcut chord)
        {
            m_chord = chord;
            StopCapture();
            ShowChord();
        }
        [[nodiscard]] bool IsCapturing() const noexcept { return m_capturing; }

        void BeginCapture()
        {
            if (m_capturing)
                return;
            m_capturing = true;
            AddClass(u8"capturing");
            SetText(u8"Press the keys (Esc cancels, Del clears)");
            RequestFocus();
            SetCapturesAllKeys(true);
        }
        void CancelCapture()
        {
            if (!m_capturing)
                return;
            StopCapture();
            ShowChord();
        }

    protected:
        void OnKeyDown(const gui::KeyEvent& event) override
        {
            if (!m_capturing)
            {
                gui::Button::OnKeyDown(event); // Return or Space clicks it, which begins a capture
                return;
            }
            event.Handled = true;
            const auto key = static_cast<gui::KeyCode>(event.KeyCode);
            if (key == gui::KeyCode::Escape && event.Modifiers == 0)
                CancelCapture();
            else if ((key == gui::KeyCode::Backspace || key == gui::KeyCode::Delete) && event.Modifiers == 0)
                Choose(EditorShortcut{});
            else if (key != gui::KeyCode::Unknown && gui::ModifierOfKey(key) == gui::KeyModNone)
            {
                EditorShortcut chord;
                chord.stroke = gui::Keystroke{event.KeyCode, event.Modifiers & (gui::KeyModShift | gui::KeyModCtrl | gui::KeyModAlt | gui::KeyModSuper), false};
                Choose(chord);
            }
            // a modifier alone, or a key the shell could not name, is not a chord: it keeps waiting
        }
        void OnFocusLost() override
        {
            CancelCapture();
            gui::Button::OnFocusLost();
        }

    private:
        void StopCapture()
        {
            if (!m_capturing)
                return;
            m_capturing = false;
            RemoveClass(u8"capturing");
            SetCapturesAllKeys(false);
        }
        void Choose(EditorShortcut chord)
        {
            StopCapture();
            m_chord = chord;
            ShowChord();
            if (m_onChosen)
                m_onChosen(chord);
        }
        void ShowChord()
        {
            const String text = FormatShortcut(m_chord);
            SetText(text.IsEmpty() ? StringView(u8"Not set") : text.AsView());
        }

        EditorShortcut m_chord;
        bool m_capturing = false;
        Function<void(EditorShortcut)> m_onChosen;
    };

    RTTI_DEFINE_OBJECT(ShortcutCaptureButton, "rtti::editor::editor::app")
}
