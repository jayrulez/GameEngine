// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the export dialog over a presets controller and a scratch templates root: the
// presets listed and the selected one's settings in tabs under the card naming the template it
// exports with (or why none), edits applied as made and saved when the selection moves, add,
// duplicate and remove, and Export running the selected preset after saving.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.ui;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace ui = foundation::ui;

namespace
{
    struct Fixture
    {
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_export_dialog_root");
        EditorContext context{DefaultAllocator()};
        ExportPresetsController presets;
        u32 saves = 0;
        Array<String> exported{DefaultAllocator()};
        bool exportedAll = false;

        Fixture()
        {
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
            ExportPreset windows;
            windows.name = String(u8"Windows");
            windows.platform = String(u8"Win64");
            (void)presets.Add(windows);
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
    };
}

TEST_CASE("export dialog: the presets, their tabs, and the template each exports with")
{
    Fixture f;
    auto dialog = MakeRef<app::ExportDialog>(DefaultAllocator(), f.context, f.presets, f.Seams());
    REQUIRE(dialog->PresetCount() == 2u);
    CHECK(dialog->SelectedIndex() == 0);
    CHECK(dialog->TabIndexOf(u8"General") == 0);
    CHECK(dialog->TabIndexOf(u8"Content") == 1);
    CHECK(dialog->TabIndexOf(u8"Display") == 2);
    CHECK(dialog->NameField().Text() == StringView(u8"Linux"));

    // Linux resolves to the installed template; Windows has none here, and says so.
    REQUIRE(dialog->Resolved() != nullptr);
    CHECK(dialog->Resolved()->id == StringView(u8"t-linux"));
    CHECK(dialog->ResolveText() == StringView(u8"Exports with Linux Release"));
    dialog->Select(1);
    CHECK(dialog->Resolved() == nullptr);
    CHECK(dialog->ResolveText() == StringView(u8"No template for Win64 Release here"));

    // A template picked by name decides the platform and config.
    dialog->Select(1);
    ui::ComboBox& tmpl = dialog->TemplateField();
    REQUIRE(tmpl.ItemCount() >= 2); // "Any", then each template (the host build's among them)
    CHECK(tmpl.SelectedIndex() == 0);
    i32 installed = -1;
    for (i32 i = 1; i < tmpl.ItemCount(); ++i)
    {
        tmpl.SetSelectedIndex(i);
        installed = tmpl.SelectedText() == StringView(u8"Linux Release") ? i : installed;
    }
    REQUIRE(installed > 0);
    tmpl.SetSelectedIndex(installed);
    CHECK(f.presets.At(1).templateId == StringView(u8"t-linux"));
    CHECK(f.presets.At(1).platform == StringView(u8"Linux64"));
    REQUIRE(dialog->Resolved() != nullptr);
}

TEST_CASE("export dialog: edits apply as made and save when the selection moves; export saves first")
{
    Fixture f;
    auto dialog = MakeRef<app::ExportDialog>(DefaultAllocator(), f.context, f.presets, f.Seams());
    REQUIRE(dialog->SelectedIndex() == 0);

    dialog->NameField().SetText(u8"Linux Desktop");
    dialog->NameField().OnTextChanged.Invoke(&dialog->NameField()); // as typing does
    dialog->PruneField().IsChecked.SetValue(true);
    CHECK(f.presets.At(0).name == StringView(u8"Linux Desktop"));
    CHECK(f.presets.At(0).pruneToReachable);
    CHECK(f.saves == 0u);
    dialog->Select(1);
    CHECK(f.saves == 1u); // saved as the selection moved
    dialog->Select(0);
    CHECK(f.saves == 1u); // nothing new to save

    // Export saves what is pending, then runs the selected preset.
    dialog->PruneField().IsChecked.SetValue(false);
    dialog->ExportSelected();
    CHECK(f.saves == 2u);
    REQUIRE(f.exported.Size() == 1u);
    CHECK(f.exported[0] == StringView(u8"Linux Desktop"));
    CHECK_FALSE(f.exportedAll);
}

TEST_CASE("export dialog: add, duplicate and remove presets")
{
    Fixture f;
    auto dialog = MakeRef<app::ExportDialog>(DefaultAllocator(), f.context, f.presets, f.Seams());

    dialog->AddPreset();
    CHECK(dialog->PresetCount() == 3u);
    CHECK(dialog->SelectedIndex() == 2);
    CHECK(f.presets.At(2).platform == StringView(u8"Linux64")); // the host's
    CHECK(dialog->NameField().Text() == StringView(u8"New Preset"));

    dialog->Select(0);
    dialog->DuplicateSelected();
    CHECK(dialog->PresetCount() == 4u);
    CHECK(dialog->SelectedIndex() == 3);
    CHECK(f.presets.At(3).name == StringView(u8"Linux Copy"));

    dialog->RemoveSelected();
    CHECK(dialog->PresetCount() == 3u);
    CHECK(dialog->SelectedIndex() == 2);
    CHECK(f.saves >= 3u);

    dialog->ExportAll();
    CHECK(f.exportedAll);
}
