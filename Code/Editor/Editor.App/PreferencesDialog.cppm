// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :preferences_dialog partition.
//
// EditorPreferencesDialog: the user's own editor settings (the foundation.settings store at
// <user-data>/editor.settings.xml), distinct from Project Settings, which edits the project
// (Documentation/Specs/editor-gui.md, 5.1, layer 12). A modal window of the editor's markup
// (Data/Editor/Preferences.gml), a tab per category. Appearance holds the look's inputs: the
// scheme, the accent (one of the editor's or any #rrggbb), the density, less motion and the UI
// scale; each change shows at once. Shortcuts lists every action with its chord: a click captures
// the next keys, Reset goes back to the declaration's. Save writes everything into the store
// (a chord another action holds refused, named in a warning) and persists it; Cancel, or closing
// the window, puts the look back as it was and forgets the shortcuts' edits.
module;
#include "Core/Prelude.h"

export module editor.app:preferences_dialog;

import foundation.core;
import foundation.gui;
import foundation.settings;
import editor.core;
import :editor_window; // EditorLook, LookFrom, StoreLook, the accents and densities
import :shortcut_capture;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    class EditorPreferencesDialog
    {
    public:
        /// Its markup, under the data root.
        static constexpr const char8_t* kPath = u8"Editor/Preferences.gml";

        /// What it works over.
        struct Services
        {
            foundation::settings::Settings* user = nullptr; // the user's store
            editor::EditorActionRegistry* actions = nullptr; // the Shortcuts tab's
            /// Says a warning to the user (a chord another action holds).
            Function<void(StringView)> warn;
            /// Shows a look (the window's SetLook).
            Function<void(const EditorLook&)> showLook;
            /// Persists the user's store; false when it could not be written.
            Function<bool()> persist;
        };

        EditorPreferencesDialog(IAllocator& allocator, Services services, RefPtr<gui::Node> body)
            : m_allocator(allocator), m_services(Move(services)), m_body(Move(body))
        {
            if (!m_body)
                return;
            m_window = MakeRef<gui::Window>(m_allocator);
            m_window->SetTitle(u8"Preferences");
            m_window->AddClass(u8"preferences-window");
            m_window->SetSize(Float2{kWidth, kHeight});
            m_window->GetContent()->AddChild(m_body.Get());
            Bind();
        }
        EditorPreferencesDialog(const EditorPreferencesDialog&) = delete;
        EditorPreferencesDialog& operator=(const EditorPreferencesDialog&) = delete;
        ~EditorPreferencesDialog() { Close(); }

        /// Opens it over `root`, its controls as the store has them; false with no markup.
        bool Open(gui::Node& root)
        {
            if (!m_window || m_window->IsOpen())
                return false;
            m_original = LookFrom(m_services.user != nullptr ? m_services.user->Find<editor::EditorUiSettings>() : nullptr);
            m_look = m_original;
            m_saved = false;
            m_shortcutEdits = editor::ShortcutEdits{};
            ShowControls();
            ShowShortcuts();
            m_window->OpenModal(root);
            return true;
        }
        [[nodiscard]] bool IsOpen() const noexcept { return m_window && m_window->IsOpen(); }

        /// Keeps the look: into the store, persisted, the window closed.
        bool Save()
        {
            if (m_services.user != nullptr)
            {
                StoreLook(m_services.user->Section<editor::EditorUiSettings>(), m_look);
                m_services.user->MarkChanged<editor::EditorUiSettings>();
                if (!m_shortcutEdits.IsEmpty() && m_services.actions != nullptr)
                {
                    Array<String> collisions;
                    (void)m_shortcutEdits.Apply(*m_services.actions, m_services.user->Section<editor::EditorShortcutSettings>(), &collisions);
                    m_services.user->MarkChanged<editor::EditorShortcutSettings>();
                    for (const String& collision : collisions)
                        if (m_services.warn)
                            m_services.warn(Format(u8"Shortcut kept: {}", collision).AsView());
                }
            }
            const bool persisted = !m_services.persist || m_services.persist();
            m_saved = true;
            if (m_window)
                m_window->Close();
            return persisted;
        }
        /// Puts the look back as it was and closes.
        void Cancel()
        {
            if (m_window && m_window->IsOpen())
                m_window->Close(); // its close puts the look back
        }

        /// The look as the controls have it now.
        [[nodiscard]] const EditorLook& Look() const noexcept { return m_look; }
        /// The window (its controls by id, for a test).
        [[nodiscard]] gui::Window* Window() const noexcept { return m_window.Get(); }

    private:
        static constexpr f32 kWidth = 560.0f;
        static constexpr f32 kHeight = 440.0f;

        void Close()
        {
            if (m_window && m_window->IsOpen())
                m_window->Close();
        }

        template <typename T>
        [[nodiscard]] T* Find(StringView id) const
        {
            return m_body ? Cast<T>(FindIn(*m_body, id)) : nullptr;
        }
        static gui::Node* FindIn(gui::Node& node, StringView id)
        {
            if (const auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
                return &node;
            for (usize i = 0; i < node.ChildCount(); ++i)
                if (gui::Node* found = FindIn(*node.GetChildAt(i), id))
                    return found;
            return nullptr;
        }

        // The controls, once: each change of the user's shows the look at once.
        void Bind()
        {
            EditorPreferencesDialog* self = this;
            const auto onChange = [self](gui::Node* node, Function<void()> apply)
            {
                if (node != nullptr)
                    (void)node->AddEventListener(gui::EventType::Changed, [self, apply = Move(apply)](const gui::Event&)
                                                 {
                                                     if (!self->m_showing)
                                                         apply();
                                                 });
            };
            onChange(Find<gui::Button>(u8"pref-dark"), [self]() { self->Change([](EditorLook& look) { look.dark = true; }); });
            onChange(Find<gui::Button>(u8"pref-light"), [self]() { self->Change([](EditorLook& look) { look.dark = false; }); });
            if (gui::Node* accents = Find<gui::Node>(u8"pref-accents"))
                for (const EditorAccent& accent : kEditorAccents)
                {
                    RefPtr<gui::Button> segment = MakeRef<gui::Button>(m_allocator);
                    segment->AddClass(u8"segment");
                    segment->SetText(accent.name);
                    segment->SetToggle(true);
                    m_accentGroup = m_accentGroup ? m_accentGroup : MakeRef<gui::ButtonGroup>(m_allocator);
                    segment->SetGroup(m_accentGroup);
                    const String fill(accent.fill);
                    onChange(segment.Get(), [self, fill]() { self->Change([&fill](EditorLook& look) { look.accent = fill; }); });
                    accents->AddChild(segment.Get());
                    m_accents.PushBack(segment);
                }
            if (auto* field = Find<gui::TextField>(u8"pref-accent"))
                field->SetOnTextChanging([self](StringView text)
                                         {
                                             if (!self->m_showing && IsHexColor(text))
                                                 self->Change([&text](EditorLook& look) { look.accent = String(text); });
                                         });
            for (StringView density : kEditorDensities)
            {
                const String name(density);
                onChange(Find<gui::Button>(Format(u8"pref-density-{}", density).AsView()),
                         [self, name]() { self->Change([&name](EditorLook& look) { look.density = name; }); });
            }
            onChange(Find<gui::CheckBox>(u8"pref-motion"), [self]()
                     {
                         const auto* motion = self->Find<gui::CheckBox>(u8"pref-motion");
                         self->Change([motion](EditorLook& look) { look.reduceMotion = motion->IsChecked(); });
                     });
            onChange(Find<gui::Slider>(u8"pref-scale"), [self]()
                     {
                         const auto* scale = self->Find<gui::Slider>(u8"pref-scale");
                         self->Change([scale](EditorLook& look) { look.uiScale = static_cast<f32>(scale->GetValue()); });
                     });
            if (auto* save = Find<gui::Button>(u8"pref-save"))
                save->SetOnClick([self]() { (void)self->Save(); });
            if (auto* cancel = Find<gui::Button>(u8"pref-cancel"))
                cancel->SetOnClick([self]() { self->Cancel(); });
            // However it closes (Cancel, its close button, Escape), unsaved, the look goes back.
            m_window->SetOnClose([self]()
                                 {
                                     if (!self->m_saved && self->m_services.showLook)
                                         self->m_services.showLook(self->m_original);
                                 });
        }

        void Change(const Function<void(EditorLook&)>& edit)
        {
            edit(m_look);
            m_look.uiScale = Clamp(m_look.uiScale, editor::kUiScaleMin, editor::kUiScaleMax);
            ShowControls();
            if (m_services.showLook)
                m_services.showLook(m_look);
        }

        // The Shortcuts tab: a row per action, its chord as the registry has it now.
        void ShowShortcuts()
        {
            gui::Node* list = Find<gui::Node>(u8"pref-shortcuts");
            if (list == nullptr || m_services.actions == nullptr)
                return;
            while (list->ChildCount() > 0)
                list->RemoveChild(list->GetChildAt(list->ChildCount() - 1));
            editor::EditorActionRegistry& actions = *m_services.actions;
            EditorPreferencesDialog* self = this;
            for (const editor::EditorActionDeclaration& action : actions.Actions())
            {
                RefPtr<gui::Panel> row = MakeRef<gui::Panel>(m_allocator);
                row->AddClass(u8"pref-shortcut");
                RefPtr<gui::Label> name = MakeRef<gui::Label>(m_allocator);
                name->AddClass(u8"row-name");
                name->SetText(action.label.AsView());
                RefPtr<gui::Label> where = MakeRef<gui::Label>(m_allocator);
                where->AddClass(u8"pref-where");
                where->SetText(action.menuPath.AsView());
                RefPtr<ShortcutCaptureButton> capture = MakeRef<ShortcutCaptureButton>(m_allocator);
                capture->SetChord(actions.Shortcut(action.id.AsView()));
                RefPtr<gui::Button> reset = MakeRef<gui::Button>(m_allocator);
                reset->AddClass(u8"ghost");
                reset->SetText(u8"Reset");
                reset->SetEnabled(actions.HasOverride(action.id.AsView()));
                const String id = action.id;
                const EditorShortcut declared = action.shortcut;
                gui::Button* resetRaw = reset.Get();
                ShortcutCaptureButton* captureRaw = capture.Get();
                capture->SetOnChordChosen([self, id, resetRaw](EditorShortcut chord)
                                          {
                                              self->m_shortcutEdits.Set(id.AsView(), chord);
                                              resetRaw->SetEnabled(true);
                                          });
                reset->SetOnClick([self, id, captureRaw, resetRaw, declared]()
                                  {
                                      self->m_shortcutEdits.Reset(id.AsView());
                                      captureRaw->SetChord(declared); // the default, shown at once
                                      resetRaw->SetEnabled(false);
                                  });
                row->AddChild(name.Get());
                row->AddChild(where.Get());
                row->AddChild(capture.Get());
                row->AddChild(reset.Get());
                list->AddChild(row.Get());
            }
        }

        // The controls as m_look has it (quietly: no change of theirs comes back).
        void ShowControls()
        {
            m_showing = true;
            if (auto* dark = Find<gui::Button>(u8"pref-dark"))
                dark->SetChecked(m_look.dark);
            if (auto* light = Find<gui::Button>(u8"pref-light"))
                light->SetChecked(!m_look.dark);
            for (usize i = 0; i < m_accents.Size(); ++i)
                m_accents[i]->SetChecked(m_look.accent.AsView() == kEditorAccents[i].fill);
            if (auto* field = Find<gui::TextField>(u8"pref-accent"); field != nullptr && field->GetText() != m_look.accent.AsView())
                field->SetText(m_look.accent.AsView());
            for (StringView density : kEditorDensities)
                if (auto* segment = Find<gui::Button>(Format(u8"pref-density-{}", density).AsView()))
                    segment->SetChecked(m_look.density.AsView() == density);
            if (auto* motion = Find<gui::CheckBox>(u8"pref-motion"))
                motion->SetChecked(m_look.reduceMotion);
            if (auto* scale = Find<gui::Slider>(u8"pref-scale"))
                scale->SetValue(m_look.uiScale);
            if (auto* value = Find<gui::Label>(u8"pref-scale-value"))
                value->SetText(Format(u8"{}%", static_cast<i32>(m_look.uiScale * 100.0f + 0.5f)).AsView());
            m_showing = false;
        }

        IAllocator& m_allocator;
        Services m_services;
        RefPtr<gui::Node> m_body;
        RefPtr<gui::Window> m_window;
        RefPtr<gui::ButtonGroup> m_accentGroup;
        Array<RefPtr<gui::Button>> m_accents;
        EditorLook m_original; // as it was when it opened
        EditorLook m_look;     // as the controls have it
        editor::ShortcutEdits m_shortcutEdits; // staged; applied and persisted on Save
        bool m_saved = false;
        bool m_showing = false; // the controls being set from code
    };
}
