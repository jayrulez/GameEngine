// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - Preferences over its own markup (Data/Editor/Preferences.gml): its controls as the
// user's store has the look; each change shown at once; Save into the store and persisted; Cancel
// or closing putting the look back as it was; the Shortcuts tab's edits staged, applied on Save
// (a chord another action holds refused, named), Reset going back to the declaration's.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.settings;
import foundation.gui;
import editor.core;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
using editor::app::EditorLook;
using editor::app::EditorPreferencesDialog;

namespace
{
    RefPtr<gui::Node> LoadMarkup()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(),
                                     StringView(EditorPreferencesDialog::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    template <typename T>
    T* FindId(gui::Node& node, StringView id)
    {
        if (auto* widget = Cast<gui::UIWidget>(&node); widget != nullptr && widget->GetId() == id)
            return Cast<T>(&node);
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (T* found = FindId<T>(*node.GetChildAt(i), id))
                return found;
        return nullptr;
    }
    gui::Button* ButtonText(gui::Node& node, StringView text)
    {
        if (auto* button = Cast<gui::Button>(&node); button != nullptr && button->GetText() == text)
            return button;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::Button* found = ButtonText(*node.GetChildAt(i), text))
                return found;
        return nullptr;
    }

    struct Bed
    {
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        foundation::settings::Settings store{DefaultAllocator()};
        editor::EditorActionRegistry actions;
        Array<String> warnings;
        Array<EditorLook> shown;
        int persisted = 0;
        UniquePtr<EditorPreferencesDialog> dialog;
        Bed()
        {
            editor::RegisterEditorSettingsTypes();
            root->SetSize(Float2{1000.0f, 700.0f});
            store.Section<editor::EditorUiSettings>().accent = String(u8"#6d5bd8"); // violet
            const auto declare = [this](StringView id, StringView label, StringView keys)
            {
                editor::EditorActionDeclaration d;
                d.id = String(id);
                d.label = String(label);
                d.menuPath = Format(u8"File/{}", label);
                d.shortcut = editor::EditorShortcut::Of(keys);
                d.execute = [](editor::EditorPage*) {};
                REQUIRE(actions.Register(Move(d)));
            };
            declare(u8"file.save", u8"Save", u8"ctrl-s");
            declare(u8"file.open", u8"Open", u8"ctrl-o");
            EditorPreferencesDialog::Services services;
            services.user = &store;
            services.actions = &actions;
            Bed* self = this;
            services.warn = [self](StringView text) { self->warnings.PushBack(String(text)); };
            services.showLook = [self](const EditorLook& look) { self->shown.PushBack(look); };
            services.persist = [self]()
            {
                ++self->persisted;
                return true;
            };
            dialog = MakeUnique<EditorPreferencesDialog>(DefaultAllocator(), DefaultAllocator(), Move(services), LoadMarkup());
        }
        [[nodiscard]] gui::Window& Window() { return *dialog->Window(); }
    };
}

TEST_CASE("preferences: its controls as the store has the look; a change shown at once; Save into the store, persisted")
{
    Bed bed;
    REQUIRE(bed.dialog->Window() != nullptr);
    REQUIRE(bed.dialog->Open(*bed.root));
    CHECK(bed.dialog->IsOpen());
    gui::Window& window = bed.Window();
    auto* dark = FindId<gui::Button>(window, u8"pref-dark");
    auto* light = FindId<gui::Button>(window, u8"pref-light");
    auto* comfortable = FindId<gui::Button>(window, u8"pref-density-comfortable");
    auto* motion = FindId<gui::CheckBox>(window, u8"pref-motion");
    auto* accent = FindId<gui::TextField>(window, u8"pref-accent");
    REQUIRE(dark != nullptr);
    REQUIRE(light != nullptr);
    REQUIRE(comfortable != nullptr);
    REQUIRE(motion != nullptr);
    REQUIRE(accent != nullptr);
    CHECK(dark->IsChecked());
    CHECK_FALSE(light->IsChecked());
    CHECK(FindId<gui::Button>(window, u8"pref-density-compact")->IsChecked());
    CHECK(ButtonText(window, u8"Violet")->IsChecked());
    CHECK(accent->GetText() == StringView(u8"#6d5bd8"));
    CHECK(bed.shown.IsEmpty()); // nothing changed yet

    CHECK(light->HandleActivate());
    REQUIRE(bed.shown.Size() == 1u);
    CHECK_FALSE(bed.shown.Back().dark);
    CHECK_FALSE(dark->IsChecked()); // one of the two
    CHECK(ButtonText(window, u8"Teal")->HandleActivate());
    CHECK(bed.shown.Back().accent == String(u8"#0f8a7e"));
    CHECK(accent->GetText() == StringView(u8"#0f8a7e"));
    CHECK(comfortable->HandleActivate());
    CHECK(bed.shown.Back().density == String(u8"comfortable"));
    CHECK(motion->HandleActivate());
    CHECK(bed.shown.Back().reduceMotion);

    CHECK(bed.dialog->Save());
    CHECK_FALSE(bed.dialog->IsOpen());
    CHECK(bed.persisted == 1);
    const editor::EditorUiSettings& ui = *bed.store.Find<editor::EditorUiSettings>();
    CHECK(ui.scheme == String(u8"light"));
    CHECK(ui.accent == String(u8"#0f8a7e"));
    CHECK(ui.density == String(u8"comfortable"));
    CHECK(ui.reduceMotion);
    CHECK_FALSE(bed.shown.Back().dark); // saved: nothing put back
}

TEST_CASE("preferences: Cancel, or closing the window, puts the look back as it was; the store unchanged")
{
    Bed bed;
    REQUIRE(bed.dialog->Open(*bed.root));
    CHECK(FindId<gui::Button>(bed.Window(), u8"pref-light")->HandleActivate());
    CHECK_FALSE(bed.shown.Back().dark);
    bed.dialog->Cancel();
    CHECK_FALSE(bed.dialog->IsOpen());
    CHECK(bed.shown.Back().dark); // back
    CHECK(bed.store.Find<editor::EditorUiSettings>()->scheme == String(u8"dark"));
    CHECK(bed.persisted == 0);

    REQUIRE(bed.dialog->Open(*bed.root)); // opened again: as the store has it
    CHECK(FindId<gui::Button>(bed.Window(), u8"pref-dark")->IsChecked());
    CHECK(FindId<gui::Button>(bed.Window(), u8"pref-density-default")->HandleActivate());
    bed.Window().RequestClose(); // its close button, Escape
    CHECK_FALSE(bed.dialog->IsOpen());
    CHECK(bed.shown.Back().density == String(u8"compact"));
}

TEST_CASE("preferences: Shortcuts lists every action; an edit is staged and applied on Save, one another action holds refused; Reset goes back")
{
    Bed bed;
    REQUIRE(bed.dialog->Open(*bed.root));
    auto* tabs = FindId<gui::TabView>(bed.Window(), u8"pref-tabs");
    REQUIRE(tabs != nullptr);
    tabs->SetSelectedIndex(1); // Shortcuts
    (void)bed.root->UpdateLayout();
    Array<editor::app::ShortcutCaptureButton*> captures;
    const auto collect = [&captures](auto&& self, gui::Node& node) -> void
    {
        if (auto* capture = Cast<editor::app::ShortcutCaptureButton>(&node))
            captures.PushBack(capture);
        for (usize i = 0; i < node.ChildCount(); ++i)
            self(self, *node.GetChildAt(i));
    };
    collect(collect, bed.Window());
    REQUIRE(captures.Size() == 2u); // Save, Open
    CHECK(captures[0]->Chord() == editor::EditorShortcut::Of(u8"ctrl-s"));

    gui::EventDispatcher& d = *bed.root->GetEventDispatcher();
    CHECK(captures[0]->HandleActivate());
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::F2));
    CHECK(captures[1]->HandleActivate());
    d.InjectKeyDown(static_cast<u32>(gui::KeyCode::F2)); // the one Save now holds
    CHECK(bed.actions.Shortcut(u8"file.save") == editor::EditorShortcut::Of(u8"ctrl-s")); // staged, not yet
    CHECK(bed.dialog->Save());
    CHECK(bed.actions.Shortcut(u8"file.save") == editor::EditorShortcut::Of(u8"f2"));
    CHECK(bed.actions.Shortcut(u8"file.open") == editor::EditorShortcut::Of(u8"ctrl-o")); // refused
    CHECK(bed.warnings.Size() == 1u);
    CHECK(bed.store.Find<editor::EditorShortcutSettings>() != nullptr);

    // Opened again, Reset puts Save back to its declared chord on Save.
    REQUIRE(bed.dialog->Open(*bed.root));
    (void)bed.root->UpdateLayout();
    gui::Button* reset = ButtonText(bed.Window(), u8"Reset");
    REQUIRE(reset != nullptr);
    CHECK(reset->IsEnabled()); // Save has an override
    CHECK(reset->HandleActivate());
    CHECK(bed.dialog->Save());
    CHECK(bed.actions.Shortcut(u8"file.save") == editor::EditorShortcut::Of(u8"ctrl-s"));
}
