// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the export dialog over its markup (Data/Editor/Export.xml), a presets
// controller and a scratch templates root: the presets listed and the selected one's settings on
// the grid in their sections under the card naming the template it exports with (or why none),
// a template picked by name deciding the platform and config, edits applied as made and saved when
// the selection moves, an override's rows shown only while it overrides, add, duplicate and remove
// (after asking), and Export running the selected preset after saving.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadExport()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(app::ExportDialog::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }
    gui::MessageBox* FindBox(gui::Node& node)
    {
        if (auto* box = Cast<gui::MessageBox>(&node); box != nullptr && box->IsOpen())
            return box;
        for (usize i = 0; i < node.ChildCount(); ++i)
            if (gui::MessageBox* found = FindBox(*node.GetChildAt(i)))
                return found;
        return nullptr;
    }
    void Edit(app::ExportDialog& dialog, StringView path, const gui::Variant& value)
    {
        dialog.Rows()->BeginEdit(path);
        dialog.Rows()->CommitEdit(path, gui::PropertyWrite{value});
    }
}

namespace
{
    struct Fixture
    {
        RefPtr<gui::SceneRoot> ui = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_export_dialog_root");
        EditorContext context{DefaultAllocator()};
        ExportPresetsController presets;
        u32 saves = 0;
        Array<String> exported{DefaultAllocator()};
        bool exportedAll = false;

        Fixture()
        {
            ui->SetSize(Float2{1000.0f, 700.0f});
            (void)RemoveDirectoryRecursive(root.AsView());
            const String dir = PathJoin(root.AsView(), u8"t-linux");
            REQUIRE(CreateDirectories(dir.AsView()));
            foundation::vfs::NativeFileSystem fs(dir.AsView(), DefaultAllocator());
            ExportTemplate tmpl;
            tmpl.id = String(u8"t-linux");
            tmpl.name = String(u8"Linux Release");
            tmpl.platform = String(u8"Linux64");
            tmpl.config = String(u8"Release");
            tmpl.engineVersion = String(kEngineVersionString);
            tmpl.playerBinary = String(u8"Engine.Player");
            REQUIRE(SaveTemplateManifest(*fs.AsWritable(), tmpl).IsOk());

            ExportPreset linuxPreset;
            linuxPreset.name = String(u8"Linux");
            linuxPreset.platform = String(u8"Linux64");
            (void)presets.Add(linuxPreset);
            // A platform no machine running the tests hosts (the registry always adds the running
            // build as a template, so a Win64 preset finds one on a Windows runner).
            ExportPreset web;
            web.name = String(u8"Web");
            web.platform = String(u8"Web");
            (void)presets.Add(web);
        }
        ~Fixture() { (void)RemoveDirectoryRecursive(root.AsView()); }

        app::ExportDialogSeams Seams()
        {
            app::ExportDialogSeams seams;
            seams.refresh = [this](TemplateRegistry& registry)
            {
                foundation::vfs::NativeFileSystem rootFs(root.AsView(), DefaultAllocator());
                registry.Refresh(root.AsView(), &rootFs, StringView(), nullptr);
            };
            seams.save = [this]() { ++saves; };
            seams.runExport = [this](StringView preset, bool all)
            {
                exported.PushBack(String(preset));
                exportedAll = all;
            };
            seams.hostPlatform = String(u8"Linux64");
            seams.outputRoot = String(u8"/tmp/project/Dist");
            return seams;
        }
        UniquePtr<app::ExportDialog> Open()
        {
            UniquePtr<app::ExportDialog> dialog = MakeUnique<app::ExportDialog>(DefaultAllocator(), DefaultAllocator(), presets, Seams(), LoadExport());
            REQUIRE(dialog->Open(*ui));
            (void)ui->UpdateLayout();
            return dialog;
        }
    };
}

TEST_CASE("export dialog: the presets, their settings in sections, and the template each exports with")
{
    Fixture f;
    UniquePtr<app::ExportDialog> dialog = f.Open();
    REQUIRE(dialog->PresetCount() == 2u);
    CHECK(dialog->SelectedIndex() == 0);
    gui::PropertyBag& rows = *dialog->Rows();
    CHECK(rows.GetValue(u8"name", 0).AsString() == StringView(u8"Linux"));
    CHECK(rows.FindInfo(u8"name")->Category == String(u8"General"));
    CHECK(rows.FindInfo(u8"stageSymbols")->Category == String(u8"Content"));
    CHECK(rows.FindInfo(u8"orientation")->Category == String(u8"Display"));

    // Linux resolves to the installed template; Web has none here, and says so.
    REQUIRE(dialog->Resolved() != nullptr);
    CHECK(dialog->Resolved()->id == StringView(u8"t-linux"));
    CHECK(dialog->ResolveText() == StringView(u8"Exports with Linux Release"));
    CHECK(dialog->ExportButton()->IsEnabled());
    dialog->Select(1);
    CHECK(dialog->Resolved() == nullptr);
    CHECK(dialog->ResolveText() == StringView(u8"No template for Web Release here"));
    CHECK_FALSE(dialog->ExportButton()->IsEnabled());

    // A template picked by name decides the platform and config, which then are read only.
    const gui::PropertyInfo* tmpl = dialog->Rows()->FindInfo(u8"template");
    REQUIRE(tmpl != nullptr);
    REQUIRE(tmpl->Choices.Size() >= 2u); // Any, then each template
    CHECK(dialog->Rows()->GetValue(u8"template", 0).AsInt() == 0);
    i64 installed = -1;
    for (const gui::PropertyChoice& choice : tmpl->Choices)
        installed = choice.Name == String(u8"Linux Release") ? choice.Value : installed;
    REQUIRE(installed > 0);
    Edit(*dialog, u8"template", gui::Variant(installed));
    CHECK(f.presets.At(1).templateId == StringView(u8"t-linux"));
    CHECK(f.presets.At(1).platform == StringView(u8"Linux64"));
    CHECK(dialog->Rows()->FindInfo(u8"platform")->ReadOnly);
    REQUIRE(dialog->Resolved() != nullptr);
}

TEST_CASE("export dialog: edits apply as made and save when the selection moves; export saves first")
{
    Fixture f;
    UniquePtr<app::ExportDialog> dialog = f.Open();
    REQUIRE(dialog->SelectedIndex() == 0);

    Edit(*dialog, u8"name", gui::Variant(StringView(u8"Linux Desktop")));
    Edit(*dialog, u8"pruneToReachable", gui::Variant(true));
    CHECK(f.presets.At(0).name == StringView(u8"Linux Desktop"));
    CHECK(f.presets.At(0).pruneToReachable);
    CHECK(f.saves == 0u);
    dialog->Select(1);
    CHECK(f.saves == 1u); // saved as the selection moved
    dialog->Select(0);
    CHECK(f.saves == 1u); // nothing new to save

    // Export saves what is pending, then runs the selected preset.
    Edit(*dialog, u8"pruneToReachable", gui::Variant(false));
    dialog->ExportSelected();
    CHECK(f.saves == 2u);
    REQUIRE(f.exported.Size() == 1u);
    CHECK(f.exported[0] == StringView(u8"Linux Desktop"));
    CHECK_FALSE(f.exportedAll);
    CHECK_FALSE(dialog->IsOpen());
}

// Present transform P2: a preset holds the game its own way (a phone-facing Web preset says
// Landscape), and the dialog shows the preset's choice again when it is selected.
TEST_CASE("export dialog: a preset overrides the orientation, its row shown only while it does")
{
    Fixture f;
    UniquePtr<app::ExportDialog> dialog = f.Open();
    REQUIRE(dialog->SelectedIndex() == 0);
    CHECK_FALSE(dialog->Rows()->GetValue(u8"overridesOrientation", 0).AsBool());
    CHECK_FALSE(dialog->Rows()->FindInfo(u8"orientation")->Visible);

    Edit(*dialog, u8"overridesOrientation", gui::Variant(true));
    CHECK(dialog->Rows()->FindInfo(u8"orientation")->Visible);
    Edit(*dialog, u8"orientation", gui::Variant(static_cast<i64>(ScreenOrientation::Landscape)));
    CHECK(f.presets.At(0).overridesOrientation);
    CHECK(f.presets.At(0).orientation == ScreenOrientation::Landscape);
    CHECK_FALSE(f.presets.At(1).overridesOrientation);

    // The other preset shows its own; coming back shows this one's again.
    dialog->Select(1);
    CHECK_FALSE(dialog->Rows()->GetValue(u8"overridesOrientation", 0).AsBool());
    dialog->Select(0);
    CHECK(dialog->Rows()->GetValue(u8"overridesOrientation", 0).AsBool());
    CHECK(dialog->Rows()->GetValue(u8"orientation", 0).AsInt() == static_cast<i64>(ScreenOrientation::Landscape));
}

TEST_CASE("export dialog: add, duplicate, and remove after asking")
{
    Fixture f;
    UniquePtr<app::ExportDialog> dialog = f.Open();

    dialog->AddPreset();
    CHECK(dialog->PresetCount() == 3u);
    CHECK(dialog->SelectedIndex() == 2);
    CHECK(f.presets.At(2).platform == StringView(u8"Linux64")); // the host's
    CHECK(dialog->Rows()->GetValue(u8"name", 0).AsString() == StringView(u8"New Preset"));

    dialog->Select(0);
    dialog->DuplicateSelected();
    CHECK(dialog->PresetCount() == 4u);
    CHECK(dialog->SelectedIndex() == 3);
    CHECK(f.presets.At(3).name == StringView(u8"Linux Copy"));

    // Remove asks; Escape keeps it, Remove removes it.
    dialog->RemoveSelected();
    gui::MessageBox* box = FindBox(*f.ui);
    REQUIRE(box != nullptr);
    box->RequestClose();
    CHECK(dialog->PresetCount() == 4u);
    dialog->RemoveSelected();
    box = FindBox(*f.ui);
    REQUIRE(box != nullptr);
    REQUIRE(box->ButtonAt(1) != nullptr);
    CHECK(box->ButtonAt(1)->HandleActivate());
    CHECK(dialog->PresetCount() == 3u);
    CHECK(dialog->SelectedIndex() == 2);
    CHECK(f.saves >= 3u);

    dialog->ExportAll();
    CHECK(f.exportedAll);
}
