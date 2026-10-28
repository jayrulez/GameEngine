// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Project Settings over its markup (Data/Editor/ProjectSettings.xml): the manifest's labelled
// settings as the grid's rows, in their categories; an asset setting a reference to the source
// type it names (its slot), a list setting a list of them, an enum its choices by name, MSAA the
// render subsystem's levels, the project's engine version read only; Save writing the rows back
// (a list's empty and repeated entries dropped) and persisting, Cancel leaving the manifest as it
// was.

#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.scene.resource;
import engine.project;
import editor.core;
import editor.app;

using namespace foundation::core;
using editor::app::ProjectSettingsDialog;
using editor::app::ResourceRefEditor;
using editor::app::ResourceRefServices;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadSettings()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(editor::app::kProjectSettingsDialogPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    String Text(const Guid& id)
    {
        utf8char chars[37];
        id.ToChars(chars);
        return String(StringView(chars, 36));
    }
    struct Bed
    {
        UniquePtr<editor::EditorProject> project;
        editor::EditorContext context{DefaultAllocator()};
        ResourceRefServices refs;
        RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        UniquePtr<ProjectSettingsDialog> dialog;
        const Guid scene{0x5ce0u, 0x1u};
        const Guid font{0xf0u, 0x2u};

        Bed()
        {
            std::error_code ec;
            std::filesystem::remove_all("project_settings_project", ec);
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), u8"project_settings_project", u8"Settings").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), u8"project_settings_project");
            REQUIRE(project);
            GlobalTypeRegistry().Register(foundation::scene::SceneDocument::StaticType());
            project->Settings().defaultSceneId = scene;
            project->Settings().uiFontIds.PushBack(font);
            project->Settings().renderMsaaSamples = 4;
            context.SetProject(project.Get());
            root->SetSize(Float2{900.0f, 700.0f});
            dialog = MakeUnique<ProjectSettingsDialog>(DefaultAllocator(), DefaultAllocator(), context, refs, LoadSettings());
            REQUIRE(dialog->Open(*root));
            (void)root->UpdateLayout();
        }
        ~Bed()
        {
            dialog.Reset();
            context.SetProject(nullptr);
            project.Reset();
            std::error_code ec;
            std::filesystem::remove_all("project_settings_project", ec);
        }
        gui::PropertyBag& Bag() { return *dialog->Rows(); }
        void Edit(StringView path, const gui::Variant& value)
        {
            Bag().BeginEdit(path);
            Bag().CommitEdit(path, gui::PropertyWrite{value});
        }
    };
}

TEST_CASE("project settings: the labelled settings as rows in their categories; assets as references to the type they name; choices by name")
{
    Bed bed;
    gui::PropertyBag& bag = bed.Bag();
    const gui::PropertyInfo* name = bag.FindInfo(u8"name");
    REQUIRE(name != nullptr);
    CHECK(name->DisplayName == String(u8"Name"));
    CHECK(name->Category == String(u8"General"));
    CHECK(bag.GetValue(u8"name", 0).AsString() == StringView(u8"Settings"));

    const gui::PropertyInfo* scene = bag.FindInfo(u8"defaultSceneId");
    REQUIRE(scene != nullptr);
    CHECK(scene->Kind == gui::PropertyKind::Reference);
    CHECK(scene->ReferenceType == &foundation::scene::SceneDocument::StaticType());
    CHECK(scene->Category == String(u8"Startup"));
    CHECK(bag.GetValue(u8"defaultSceneId", 0).AsString() == Text(bed.scene).AsView());
    gui::PropertyEditorRegistry& editors = bed.dialog->Grid()->GetEditors();
    CHECK(Cast<ResourceRefEditor>(editors.Create(editors.Choose(*scene), DefaultAllocator()).Get()) != nullptr); // its slot

    const gui::PropertyInfo* fonts = bag.FindInfo(u8"uiFontIds");
    REQUIRE(fonts != nullptr);
    CHECK(fonts->Kind == gui::PropertyKind::List);
    CHECK(bag.ElementCount(u8"uiFontIds", 0) == 1u);
    CHECK(bag.GetValue(u8"uiFontIds.0", 0).AsString() == Text(bed.font).AsView());

    const gui::PropertyInfo* msaa = bag.FindInfo(editor::app::kProjectSettingsDialogMsaaSetting);
    REQUIRE(msaa != nullptr);
    CHECK(msaa->Kind == gui::PropertyKind::Enum);
    REQUIRE(msaa->Choices.Size() == 3u);
    CHECK(msaa->Choices[2].Name == String(u8"4x"));
    CHECK(bag.GetValue(editor::app::kProjectSettingsDialogMsaaSetting, 0).AsInt() == 4);

    const gui::PropertyInfo* mode = bag.FindInfo(u8"windowMode");
    REQUIRE(mode != nullptr);
    CHECK(mode->Kind == gui::PropertyKind::Enum);
    CHECK(mode->Choices[0].Name == String(u8"Windowed"));

    const gui::PropertyInfo* width = bag.FindInfo(u8"windowWidth");
    REQUIRE(width != nullptr);
    CHECK(width->HasRange);
    CHECK(width->Min == doctest::Approx(1.0));

    const gui::PropertyInfo* version = bag.FindInfo(editor::app::kProjectSettingsDialogEngineVersionRow);
    REQUIRE(version != nullptr);
    CHECK(version->ReadOnly);
}

TEST_CASE("project settings: Save writes the rows back, a list's empty and repeated entries dropped, and persists; Cancel leaves it")
{
    Bed bed;
    bed.Edit(u8"name", gui::Variant(StringView(u8"Renamed")));
    // Cancel first: the manifest is as it was.
    bed.dialog->Close();
    CHECK(bed.project->Settings().name == String(u8"Settings"));
    CHECK(bed.project->Settings().defaultSceneId == bed.scene);

    REQUIRE(bed.dialog->Open(*bed.root));
    CHECK(bed.Bag().GetValue(u8"name", 0).AsString() == StringView(u8"Settings")); // the rows again, from the manifest
    bed.Edit(u8"name", gui::Variant(StringView(u8"Renamed")));
    bed.Edit(u8"defaultSceneId", gui::Variant(StringView{}));
    bed.Edit(editor::app::kProjectSettingsDialogMsaaSetting, gui::Variant(i64{2}));
    bed.Bag().ResizeList(u8"uiFontIds", 0, 3);
    bed.Bag().SetValue(u8"uiFontIds.1", 0, gui::Variant(Text(bed.font).AsView()));
    REQUIRE(bed.dialog->Save());
    CHECK_FALSE(bed.dialog->IsOpen());
    const engine::project::ProjectSettings& saved = bed.project->Settings();
    CHECK(saved.name == String(u8"Renamed"));
    CHECK(saved.defaultSceneId.IsNil());
    CHECK(saved.renderMsaaSamples == 2u);
    REQUIRE(saved.uiFontIds.Size() == 1u);
    CHECK(saved.uiFontIds[0] == bed.font);

    // Persisted: a project opened again reads it.
    UniquePtr<editor::EditorProject> again = editor::EditorProject::Open(DefaultAllocator(), u8"project_settings_project");
    REQUIRE(again);
    CHECK(again->Settings().name == String(u8"Renamed"));
}
