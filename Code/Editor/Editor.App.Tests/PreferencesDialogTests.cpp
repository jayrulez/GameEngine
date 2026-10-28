// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - Preferences over its own markup (Data/Editor/Preferences.xml): its controls as the
// user's store has the look; each change shown at once; Save into the store and persisted; Cancel
// or closing putting the look back as it was; the Shortcuts tab's edits staged, applied on Save
// (a chord another action holds refused, named), Reset going back to the declaration's; the
// orthographic check, the templates' folder, the MCP host's fields, the preview resolutions and
// a contributed category's tab.
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
        Array<editor::EditorContext::EditorSettingsContribution> contributions;
        bool snapping = true; // a contributed setting
        int mcpChanged = 0;
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
            editor::EditorContext::EditorSettingsContribution navigation;
            navigation.category = String(u8"Navigation");
            editor::EditorContext::EditorSettingsBoolField snap;
            snap.label = String(u8"Snap agents to the mesh");
            snap.get = [self]() { return self->snapping; };
            snap.set = [self](bool on) { self->snapping = on; };
            navigation.bools.PushBack(Move(snap));
            contributions.PushBack(Move(navigation));
            services.contributions = &contributions;
            services.mcpChanged = [self]() { ++self->mcpChanged; };
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
    for (i32 i = 0; i < tabs->GetPageCount(); ++i)
        if (auto* page = Cast<gui::TabPage>(tabs->GetPage(i)); page != nullptr && page->GetTitle() == StringView(u8"Shortcuts"))
            tabs->SetSelectedIndex(i);
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

TEST_CASE("preferences: the other tabs - axis views, templates, the MCP host (a bad port kept), preview sizes, a contributed category")
{
    Bed bed;
    bed.store.Section<editor::EditorMcpSettings>().port = 7500;
    REQUIRE(bed.dialog->Open(*bed.root));
    gui::Window& window = bed.Window();
    auto* tabs = FindId<gui::TabView>(window, u8"pref-tabs");
    REQUIRE(tabs != nullptr);
    CHECK(tabs->GetPageCount() == 6); // Appearance, Export, Agent access, Game preview, Shortcuts, Navigation
    auto* ortho = FindId<gui::CheckBox>(window, u8"pref-auto-ortho");
    auto* root = FindId<gui::TextField>(window, u8"pref-templates-root");
    auto* enabled = FindId<gui::CheckBox>(window, u8"pref-mcp-enabled");
    auto* port = FindId<gui::TextField>(window, u8"pref-mcp-port");
    REQUIRE(ortho != nullptr);
    REQUIRE(root != nullptr);
    REQUIRE(enabled != nullptr);
    REQUIRE(port != nullptr);
    CHECK(ortho->IsChecked());
    CHECK(port->GetText() == StringView(u8"7500"));
    CHECK_FALSE(enabled->IsChecked());

    // Six preview sizes seeded; one removed, one added.
    gui::Node* previews = FindId<gui::Node>(window, u8"pref-previews");
    REQUIRE(previews != nullptr);
    CHECK(previews->ChildCount() == 6u);
    gui::Button* remove = ButtonText(*previews->GetChildAt(0), u8"Remove");
    REQUIRE(remove != nullptr);
    CHECK(remove->HandleActivate());
    CHECK(FindId<gui::Button>(window, u8"pref-preview-add")->HandleActivate());
    CHECK(previews->ChildCount() == 7u);

    // The contributed check applies as it changes.
    gui::CheckBox* snap = nullptr;
    const auto find = [&snap](auto&& self, gui::Node& node) -> void
    {
        if (auto* box = Cast<gui::CheckBox>(&node); box != nullptr && box->GetText() == StringView(u8"Snap agents to the mesh"))
            snap = box;
        for (usize i = 0; i < node.ChildCount(); ++i)
            self(self, *node.GetChildAt(i));
    };
    find(find, window);
    REQUIRE(snap != nullptr);
    CHECK(snap->IsChecked());
    CHECK(snap->HandleActivate());
    CHECK_FALSE(bed.snapping);

    CHECK(ortho->HandleActivate());
    root->SetText(u8"/opt/templates");
    CHECK(enabled->HandleActivate());
    port->SetText(u8"80"); // below the range
    CHECK(bed.dialog->Save());
    CHECK_FALSE(bed.store.Find<editor::EditorUiSettings>()->autoOrthographic);
    CHECK(bed.store.Find<editor::EditorExportSettings>()->templatesRoot == String(u8"/opt/templates"));
    const editor::EditorMcpSettings& mcp = *bed.store.Find<editor::EditorMcpSettings>();
    CHECK(mcp.enabled);
    CHECK(mcp.port == 7500u); // kept
    CHECK(bed.warnings.Size() == 1u);
    CHECK(bed.mcpChanged == 1);
    const editor::GamePreviewSettings* sizes = bed.store.Find<editor::GamePreviewSettings>();
    REQUIRE(sizes != nullptr);
    CHECK(sizes->presets.Size() == 6u); // six, less one, plus the new one
    CHECK(sizes->presets[0].name != String(u8"HD"));
    CHECK(sizes->presets.Back().name == String(u8"Custom"));
}
