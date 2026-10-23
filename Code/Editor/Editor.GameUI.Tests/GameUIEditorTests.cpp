// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The game-UI editor's headless halves: the markup probe both pages show in their code
// editors, the stock preview markup, and the linked-source read without a project. Ported
// back from the Beef port's Editor.GameUI.Tests (2026-09-20).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;
import editor.core;
import editor.gameui;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.ui.resource;
import foundation.vg.resource;

using namespace foundation::core;
namespace toolkit = foundation::ui::toolkit;

TEST_CASE("gameui editor: the probe reports one error on the failing line, and none when clean")
{
    Array<toolkit::CodeDiagnostic> diagnostics;
    CHECK(editor::markup_diagnostics::Probe(DefaultAllocator(),
                                            u8"<Panel>\n  <Label text=\"a\"/>\n</Panel>", diagnostics));
    CHECK(diagnostics.IsEmpty());

    CHECK_FALSE(editor::markup_diagnostics::Probe(
        DefaultAllocator(), u8"<Panel>\n  <Label text=\"a\">\n</Panel>", diagnostics));
    REQUIRE(diagnostics.Size() == 1u);
    CHECK(diagnostics[0].isError);
    CHECK(diagnostics[0].line >= 1); // past the first line: the parser named a real line
    CHECK_FALSE(diagnostics[0].message.IsEmpty());
}

TEST_CASE("gameui editor: through an editor, the margin takes the diagnostic and drops it when clean")
{
    auto editorRef = MakeRef<toolkit::CodeEditView>(DefaultAllocator());
    toolkit::CodeEditView& editor = *editorRef;
    editor.SetText(u8"<a><b></a>");
    CHECK_FALSE(editor::markup_diagnostics::Apply(DefaultAllocator(), u8"<a><b></a>", editor));
    CHECK(editor.Document().Diagnostics().Size() == 1u);
    CHECK(editor::markup_diagnostics::Apply(DefaultAllocator(), u8"<a/>", editor));
    CHECK(editor.Document().Diagnostics().Size() == 0u);

    // The stock preview a new theme previews against is itself clean markup.
    CHECK(editor::kStockPreviewMarkup.StartsWith(u8"<Panel"));
    Array<toolkit::CodeDiagnostic> diagnostics;
    CHECK(editor::markup_diagnostics::Probe(DefaultAllocator(), editor::kStockPreviewMarkup, diagnostics));
}

TEST_CASE("gameui editor: a linked source reads empty without a project")
{
    editor::EditorContext context{DefaultAllocator()};
    const String text = editor::ReadProjectSource(context, u8"UI/missing.sml");
    CHECK(text.IsEmpty());
}

// The theme page previews a stylesheet before it is cooked: its @icon names a vector image the
// preview reads through the resource manager, so svg(name) draws there as it will in the game.
TEST_CASE("gameui editor: the theme preview reads an @icon's vector image through the resources")
{
    foundation::ui::RegisterUIResource();
    foundation::vg::RegisterVGResource();
    const StringView dir = u8"scratch_theme_preview_db";
    (void)RemoveDirectoryRecursive(dir);
    REQUIRE(CreateDirectories(dir));
    {
        foundation::vfs::NativeFileSystem mount(dir, DefaultAllocator());
        foundation::content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        foundation::content::Instance* instance =
            db.RootGroup()->CreateInstance(u8"heart", foundation::vg::VectorImageSource::StaticType());
        foundation::vg::VectorImageSource source;
        source.svg = String(u8"<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"9\" fill=\"#E53935\"/></svg>");
        REQUIRE(instance->WriteObject(source).IsOk());

        foundation::vg::VectorImageFactory factory(DefaultAllocator());
        foundation::resource::ResourceManager manager(DefaultAllocator(), db);
        manager.AddFactory(&factory);
        editor::ThemePreviewResources resources(&manager, nullptr);

        const String reference = Format(u8"{{{}}}", instance->Id());
        String svg;
        REQUIRE(resources.LoadText(reference.AsView(), svg));
        CHECK(svg.AsView() == source.svg.AsView());
        CHECK_FALSE(resources.LoadText(u8"{6dd1ae0e-fbe8-4c9b-8c9e-d10b727f4d84}", svg)); // no such asset
        CHECK_FALSE(resources.LoadText(u8"icons/heart.svg", svg));                       // not an id
        CHECK(resources.LoadImage(u8"{6dd1ae0e-fbe8-4c9b-8c9e-d10b727f4d84}") == nullptr);  // no image source

        // Through the loader, the icon becomes a drawable the preview's views resolve.
        foundation::ui::StyleSheetLoader loader(DefaultAllocator());
        loader.ResourceProvider = &resources;
        RefPtr<foundation::ui::StyleSheet> sheet =
            loader.Load(Format(u8"@icon heart \"{}\";\n.heart {{ background: svg(heart); }}\n", reference).AsView());
        REQUIRE(sheet.Get() != nullptr);
        foundation::ui::UIContext ctx{DefaultAllocator()};
        auto root = MakeRef<foundation::ui::RootView>(DefaultAllocator());
        ctx.AddRootView(root.Get());
        root->SetLocalStyleSheet(sheet);
        auto panel = MakeRef<foundation::ui::Panel>(DefaultAllocator());
        panel->AddClass(u8"heart");
        root->AddView(panel.Get());
        CHECK(Cast<foundation::ui::SVGDrawable>(panel->ResolveStyleDrawable(foundation::ui::StyleProperty::Background)) != nullptr);
        ctx.RemoveRootView(root.Get());
    }
    (void)RemoveDirectoryRecursive(dir);
}
