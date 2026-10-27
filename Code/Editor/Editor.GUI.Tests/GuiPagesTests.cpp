// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The GUI asset pages over their markup without a host (no runtime GUI: no preview drawn): a
// document's code checked as the cook checks it, its problems on their lines; a sheet previewed over
// markup of the page's own (first a little of everything, which builds clean), whose edits never
// dirty the sheet and are kept in the project's editor settings, whose problems are named apart,
// and which can be a copy of a document's markup.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.settings;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.gui.code;
import gui.pipeline;
import editor.core;
import editor.app;
import editor.gui;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    String DataPath(StringView relative)
    {
        return PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
    }
    bool ReadData(StringView relative, String& out)
    {
        Result<Array<byte>> bytes = ReadFile(DataPath(relative).AsView());
        if (!bytes.HasValue())
            return false;
        out = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
        return true;
    }
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        String text;
        REQUIRE(ReadData(relative, text));
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        gui::RegisterCodeElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(text.AsView());
    }
    void WriteSource(EditorProject& project, StringView name, StringView text)
    {
        REQUIRE(WriteFile(PathJoin(project.SourcesRoot().AsView(), name).AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }

    constexpr StringView kHud = u8"<Panel class=\"hud\">\n    <Label class=\"score\" text=\"0\"/>\n</Panel>\n";
    constexpr StringView kSheet = u8".hud { padding: 8px; }\n.score { color: #ffcc00; }\n";

    // A project with a document ("Hud", Sources/Hud.gml) and a sheet ("Theme", Sources/Theme.css),
    // its editor settings the context's.
    struct Bed
    {
        String dir = String(u8"scratch_gui_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::settings::Settings store{DefaultAllocator()};
        app::CodeLanguages languages{DefaultAllocator(), [](StringView path, String& out) { return ReadData(path, out); }};
        foundation::content::Instance* hud = nullptr;
        foundation::content::Instance* theme = nullptr;

        Bed()
        {
            pipeline::RegisterGuiAssets();
            const app::AssetPageServicesSource none;
            RegisterGuiEditor(context, none); // the settings section's type
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            WriteSource(*project, u8"Hud.gml", kHud);
            WriteSource(*project, u8"Theme.css", kSheet);
            foundation::content::Group* top = project->SourceDb().RootGroup();
            hud = top->CreateInstance(u8"Hud", pipeline::GuiDocumentAsset::StaticType());
            theme = top->CreateInstance(u8"Theme", pipeline::GuiStyleSheetAsset::StaticType());
            REQUIRE(hud != nullptr);
            REQUIRE(theme != nullptr);
            pipeline::GuiDocumentAsset document;
            document.fileName = foundation::vfs::SourcePath(u8"Hud.gml");
            REQUIRE(hud->WriteObject(document).IsOk());
            pipeline::GuiStyleSheetAsset sheet;
            sheet.fileName = foundation::vfs::SourcePath(u8"Theme.css");
            REQUIRE(theme->WriteObject(sheet).IsOk());
        }
        ~Bed()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<GuiAssetEditorPage> Open(foundation::content::Instance& instance, GuiAssetEditorPage::Kind kind)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            services.languages = &languages;
            return MakeUnique<GuiAssetEditorPage>(DefaultAllocator(), context, Move(services), instance, kind);
        }
    };
}

TEST_CASE("gui pages: registered for the document and the sheet")
{
    const app::AssetPageServicesSource none;
    EditorContext context{DefaultAllocator()};
    RegisterGuiEditor(context, none);
    CHECK(context.Pages().FindFactory(pipeline::GuiDocumentAsset::StaticType()) != nullptr);
    CHECK(context.Pages().FindFactory(pipeline::GuiStyleSheetAsset::StaticType()) != nullptr);
}

TEST_CASE("gui pages: the markup a sheet is first previewed over builds clean")
{
    Array<gui::GuiDiagnostic> found;
    CHECK(pipeline::ValidateGuiDocument(gui_pages::kStockPreviewMarkup, found, DefaultAllocator()));
    CHECK(found.IsEmpty());
}

TEST_CASE("gui pages: a document's code checked as the cook checks it, its problems on their lines")
{
    Bed bed;
    UniquePtr<GuiAssetEditorPage> page = bed.Open(*bed.hud, GuiAssetEditorPage::Kind::Document);
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Code() != nullptr);
    CHECK(page->Text() == kHud);
    CHECK(page->ErrorCount() == 0u);
    CHECK(page->PreviewMarkupEditor() == nullptr); // a document is previewed as it is
    CHECK_FALSE(page->Code()->TokensOf(0).IsEmpty());

    page->Code()->SetCaret(gui::CodePosition{1, 0});
    page->Code()->Type(u8"<Label text=\"unclosed\n");
    page->CheckNow();
    CHECK(page->ErrorCount() > 0u);
    CHECK(page->Found()[0].file.IsEmpty());
    page->Commands().Undo();
    page->CheckNow();
    CHECK(page->ErrorCount() == 0u);
}

TEST_CASE("gui pages: a sheet over markup of the page's own, kept apart from the asset, its problems named apart")
{
    Bed bed;
    UniquePtr<GuiAssetEditorPage> page = bed.Open(*bed.theme, GuiAssetEditorPage::Kind::StyleSheet);
    REQUIRE(page->PreviewMarkupEditor() != nullptr);
    CHECK(page->PreviewMarkup() == gui_pages::kStockPreviewMarkup);
    CHECK(page->ErrorCount() == 0u);

    // Its edits: the preview's, not the sheet's.
    page->PreviewMarkupEditor()->SetCaret(gui::CodePosition{0, 0});
    page->PreviewMarkupEditor()->Type(u8"<Broken\n");
    CHECK_FALSE(page->IsDirty());
    CHECK_FALSE(page->Commands().CanUndo());
    page->CheckNow();
    REQUIRE(page->ErrorCount() > 0u);
    CHECK(page->Found()[0].file == gui_pages::kPreviewMarkupName);

    // Kept for the sheet in the project's editor settings.
    const GuiSheetPreviewSettings* kept = bed.store.Find<GuiSheetPreviewSettings>();
    REQUIRE(kept != nullptr);
    REQUIRE(kept->sheets.Size() == 1u);
    CHECK(kept->sheets[0].asset == bed.theme->Id());
    CHECK(kept->sheets[0].markup.AsView().StartsWith(u8"<Broken"));

    // Over a copy of a document's markup; the sheet opened again shows what was kept.
    page->PreviewOverDocument(bed.hud->Id());
    CHECK(page->PreviewMarkup() == kHud);
    CHECK(page->ErrorCount() == 0u);
    page.Reset();
    UniquePtr<GuiAssetEditorPage> again = bed.Open(*bed.theme, GuiAssetEditorPage::Kind::StyleSheet);
    CHECK(again->PreviewMarkup() == kHud);
}
