// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests - the export templates dialog over its markup (Data/Editor/Templates.xml) and a
// scratch templates root: it lists what the root holds and the host build, shows the selected one's
// details (its engine version, warned when not this editor's), installs a bundle from a folder, and
// removes one after asking (from its button or Delete on the list), never the host build.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.gui;
import foundation.fonts;
import foundation.fonts.truetype;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    /// A template bundle in `dir`: its manifest and a player file.
    void WriteBundle(StringView dir, StringView id, StringView name, StringView engineVersion)
    {
        REQUIRE(CreateDirectories(dir));
        foundation::vfs::NativeFileSystem fs(dir, DefaultAllocator());
        ExportTemplate tmpl;
        tmpl.id = String(id);
        tmpl.name = String(name);
        tmpl.platform = String(u8"Linux64");
        tmpl.config = String(u8"Release");
        tmpl.engineVersion = String(engineVersion);
        tmpl.playerBinary = String(u8"Engine.Player");
        tmpl.sidecars.PushBack(String(u8"libwgpu_native.so"));
        tmpl.notes = String(u8"for the test");
        REQUIRE(SaveTemplateManifest(*fs.AsWritable(), tmpl).IsOk());
        const StringView player = u8"#!player\n";
        REQUIRE(fs.AsWritable()
                    ->Save(u8"Engine.Player", Span<const byte>(reinterpret_cast<const byte*>(player.Data()),
                                                               player.Size()))
                    .IsOk());
    }

    RefPtr<gui::Node> LoadTemplates()
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(app::TemplatesDialog::kPath));
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
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

    struct Fixture
    {
        RefPtr<gui::SceneRoot> ui = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
        String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_templates_dialog_root");
        String source = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_templates_dialog_source");
        String hostDir = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_templates_dialog_host");
        EditorContext context{DefaultAllocator()};
        String picked; // what the folder picker answers
        Array<String> revealed{DefaultAllocator()};

        Fixture()
        {
            ui->SetSize(Float2{1000.0f, 700.0f});
            (void)RemoveDirectoryRecursive(root.AsView());
            (void)RemoveDirectoryRecursive(source.AsView());
            (void)RemoveDirectoryRecursive(hostDir.AsView());
            REQUIRE(CreateDirectories(root.AsView()));
            WriteBundle(PathJoin(root.AsView(), u8"t-linux").AsView(), u8"t-linux", u8"Linux Release",
                        kEngineVersionString);
            WriteBundle(PathJoin(root.AsView(), u8"t-old").AsView(), u8"t-old", u8"Old Linux", u8"0.0.1");
            // The host build: a player beside the (pretend) editor.
            REQUIRE(CreateDirectories(hostDir.AsView()));
            foundation::vfs::NativeFileSystem hostFs(hostDir.AsView(), DefaultAllocator());
            const StringView player = u8"#!player\n";
            REQUIRE(hostFs.AsWritable()
                        ->Save(GetExecutableName(u8"Engine.Player").AsView(),
                               Span<const byte>(reinterpret_cast<const byte*>(player.Data()), player.Size()))
                        .IsOk());
        }
        ~Fixture()
        {
            (void)RemoveDirectoryRecursive(root.AsView());
            (void)RemoveDirectoryRecursive(source.AsView());
            (void)RemoveDirectoryRecursive(hostDir.AsView());
        }

        app::TemplatesDialogSeams Seams()
        {
            app::TemplatesDialogSeams seams;
            seams.templatesRoot = [this]() { return root; };
            seams.refresh = [this](TemplateRegistry& registry)
            {
                foundation::vfs::NativeFileSystem rootFs(root.AsView(), DefaultAllocator());
                foundation::vfs::NativeFileSystem hostFs(hostDir.AsView(), DefaultAllocator());
                registry.Refresh(root.AsView(), &rootFs, hostDir.AsView(), &hostFs);
            };
            seams.pickFolder = [this](Function<void(String)> chosen)
            {
                if (!picked.IsEmpty())
                {
                    chosen(picked);
                }
            };
            seams.revealFolder = [this](StringView folder) { revealed.PushBack(String(folder)); };
            return seams;
        }

        UniquePtr<app::TemplatesDialog> Open()
        {
            UniquePtr<app::TemplatesDialog> dialog = MakeUnique<app::TemplatesDialog>(DefaultAllocator(), DefaultAllocator(), context, Seams(), LoadTemplates());
            REQUIRE(dialog->Open(*ui));
            (void)ui->UpdateLayout();
            return dialog;
        }
        static i32 IndexOf(const app::TemplatesDialog& dialog, StringView id)
        {
            for (usize i = 0; i < dialog.TemplateCount(); ++i)
            {
                if (dialog.TemplateAt(i)->id == id)
                {
                    return static_cast<i32>(i);
                }
            }
            return -1;
        }
    };
}

TEST_CASE("templates dialog: lists the root's templates and the host build, the selected one's details")
{
    Fixture f;
    UniquePtr<app::TemplatesDialog> dialog = f.Open();
    REQUIRE(dialog->TemplateCount() == 3u); // two installed, the host build
    REQUIRE(dialog->Selected() != nullptr); // the first, selected on open

    const i32 linuxIndex = Fixture::IndexOf(*dialog, u8"t-linux");
    REQUIRE(linuxIndex >= 0);
    dialog->Select(static_cast<usize>(linuxIndex));
    CHECK(dialog->DetailText(u8"Platform") == StringView(u8"Linux64"));
    CHECK(dialog->DetailText(u8"Engine") == kEngineVersionString);
    CHECK(dialog->DetailText(u8"Runtime files") == StringView(u8"libwgpu_native.so"));
    CHECK(dialog->DetailText(u8"Notes") == StringView(u8"for the test"));
    CHECK(dialog->RemoveButton()->IsEnabled());
    CHECK(dialog->RevealButton()->HandleActivate());
    REQUIRE(f.revealed.Size() == 1u);
    CHECK(f.revealed[0].AsView().EndsWith(u8"t-linux"));

    // Built for another engine: it says so, and that an export still runs.
    dialog->Select(static_cast<usize>(Fixture::IndexOf(*dialog, u8"t-old")));
    CHECK(dialog->DetailText(u8"Engine").StartsWith(u8"0.0.1 (this editor is"));

    // The host build is this editor's own: never removable.
    for (usize i = 0; i < dialog->TemplateCount(); ++i)
    {
        if (dialog->TemplateAt(i)->isHost)
        {
            dialog->Select(i);
        }
    }
    REQUIRE(dialog->Selected()->isHost);
    CHECK_FALSE(dialog->RemoveButton()->IsEnabled());
    dialog->RemoveSelected(true);
    CHECK(dialog->TemplateCount() == 3u);
}

TEST_CASE("templates dialog: a long folder wraps inside the details, never past their edge")
{
    Fixture f;
    UniquePtr<app::TemplatesDialog> dialog = f.Open();
    String sheet;
    const char8_t* const files[] = {u8"Editor/editor.css", u8"Editor/density-default.css"};
    for (const char8_t* file : files)
    {
        Result<Array<byte>> bytes = ReadFile(PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), StringView(file)).AsView());
        REQUIRE(bytes.HasValue());
        sheet += StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size());
        sheet += u8"\n";
    }
    gui::StyleManager styles;
    styles.SetStyleSheet(gui::CSSParser::Parse(sheet.AsView()));
    dialog->Select(static_cast<usize>(Fixture::IndexOf(*dialog, u8"t-linux")));
    gui::Label* folder = dialog->DetailValue(u8"Folder");
    REQUIRE(folder != nullptr);
    // One path with no spaces to break at, far wider than the window.
    String path(u8"/home");
    for (int i = 0; i < 40; ++i)
        path += u8"/averylongfoldername";
    folder->SetText(path.AsView());
    styles.ApplyTree(*f.ui);
    // A real font after the sheet, so the path has a width (the editor's own face).
    foundation::fonts::TrueTypeFonts::Initialize();
    foundation::fonts::TrueTypeFontService fonts{DefaultAllocator()};
    const String data = foundation::vfs::FindDataRoot();
    REQUIRE(fonts.LoadFont(u8"Inter", foundation::vfs::DataPath(data.AsView(), u8"Editor/Fonts/inter/Inter-Regular.ttf").AsView()) == foundation::fonts::FontLoadResult::Success);
    folder->SetFont(fonts.GetFont(u8"Inter", 13.0f));
    (void)f.ui->UpdateLayout();
    gui::Node* fields = folder->GetParent();
    REQUIRE(fields != nullptr);
    const gui::Rect box = fields->GetScreenBounds();
    const gui::Rect value = folder->GetScreenBounds();
    CHECK(value.width > 0.0f);
    CHECK(value.x + value.width <= box.x + box.width + 0.5f);
    CHECK(value.height > 30.0f); // wrapped onto several lines

    // The list's rows are two lines with room above and below (the row's own height), where the
    // view's default of one line squeezed them into each other.
    auto* list = app::FindById<gui::ListView>(*f.ui, u8"templates-list");
    REQUIRE(list != nullptr);
    CHECK(list->GetRowHeight() == doctest::Approx(52.0f));
}

TEST_CASE("templates dialog: installs a bundle from a folder and removes one, the list following")
{
    Fixture f;
    UniquePtr<app::TemplatesDialog> dialog = f.Open();
    REQUIRE(dialog->TemplateCount() == 3u);

    // Install: the picked folder's bundle is copied into the root, listed and selected.
    WriteBundle(f.source.AsView(), u8"t-new", u8"New Linux", kEngineVersionString);
    f.picked = f.source;
    dialog->InstallFromFolder();
    CHECK(dialog->TemplateCount() == 4u);
    REQUIRE(dialog->Selected() != nullptr);
    CHECK(dialog->Selected()->id == StringView(u8"t-new"));
    CHECK(DirectoryExists(PathJoin(f.root.AsView(), u8"t-new").AsView()));

    // A cancelled pick does nothing.
    f.picked = String();
    dialog->InstallFromFolder();
    CHECK(dialog->TemplateCount() == 4u);

    // Remove asks first; Cancel keeps it.
    CHECK(dialog->RemoveButton()->HandleActivate());
    gui::MessageBox* box = FindBox(*f.ui);
    REQUIRE(box != nullptr);
    box->RequestClose(); // Escape: its cancel
    CHECK(dialog->TemplateCount() == 4u);

    // Delete on the list asks the same; Remove: its folder goes, and so does its row.
    dialog->Select(static_cast<usize>(Fixture::IndexOf(*dialog, u8"t-new")));
    REQUIRE(dialog->Selected() != nullptr);
    gui::ListView* list = nullptr;
    {
        Array<gui::Node*> pending;
        pending.PushBack(dialog->Window());
        while (!pending.IsEmpty() && list == nullptr)
        {
            gui::Node* node = pending.Back();
            pending.PopBack();
            list = Cast<gui::ListView>(node);
            for (usize i = 0; i < node->ChildCount(); ++i)
                pending.PushBack(node->GetChildAt(i));
        }
    }
    REQUIRE(list != nullptr);
    list->RequestFocus();
    f.ui->GetEventDispatcher()->InjectKeyDown(static_cast<u32>(gui::KeyCode::Delete));
    box = FindBox(*f.ui);
    REQUIRE(box != nullptr);
    REQUIRE(box->ButtonAt(1) != nullptr);
    CHECK(box->ButtonAt(1)->HandleActivate()); // Remove
    CHECK(dialog->TemplateCount() == 3u);
    CHECK_FALSE(DirectoryExists(PathJoin(f.root.AsView(), u8"t-new").AsView()));
    CHECK(Fixture::IndexOf(*dialog, u8"t-new") < 0);
}
