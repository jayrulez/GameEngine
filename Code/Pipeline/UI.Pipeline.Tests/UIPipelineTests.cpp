// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Game-UI asset pipeline: author document/theme -> VALIDATING cook -> load the products
// through the factories. Bad payloads must FAIL the cook (validation is the point).
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <initializer_list>
#include <filesystem>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import pipeline.importer;
import foundation.ui;
import foundation.ui.gamekit; // UIScreen + RegisterGamekitMarkup (the <screen> HUD repro)
import foundation.ui.resource;
import ui.pipeline;
import foundation.vg.resource; // the vector image a theme's @icon names
import vg.pipeline;

using namespace foundation::core;
using namespace pipeline;
using namespace foundation::resource;
using namespace foundation::ui;
namespace content = foundation::content;

namespace
{
    // Write a loose file at `path` with `text` (creating parents assumed to exist).
    void WriteText(StringView path, StringView text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()),
                                                 text.Size()))
                    .IsOk());
    }

    // Recursive scratch-dir cleanup (instances persist as .rasset files with varying names).
    void RemoveAll(StringView dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(
            std::filesystem::path(reinterpret_cast<const char*>(String(dir).CStr())), ec);
    }

    // A fresh Sources/ tree holding one file: the text an asset LINKS through fileName (there
    // is no inline text - the linked file is the document / stylesheet).
    void StageSource(StringView sourcesRoot, StringView fileName, StringView text)
    {
        RemoveAll(sourcesRoot);
        REQUIRE(CreateDirectories(sourcesRoot));
        WriteText(PathJoin(sourcesRoot, fileName).AsView(), text);
    }

    void RemoveTree(StringView dir)
    {
        for (const utf8char* f : {u8"menu.rasset", u8"theme.rasset"})
        {
            String path(dir);
            path.Append(u8"/");
            path.Append(f);
            FileDelete(path.AsView());
        }
        RemoveDirectory(dir);
    }
}

TEST_CASE("ui.pipeline: document + theme cook (validated) and load as products")
{
    RegisterUIResource();
    RegisterUIAssets();
    RemoveTree(u8"scratch_uipipe_db");
    foundation::vfs::NativeFileSystem outMount(u8"scratch_uipipe_db", foundation::core::DefaultAllocator());
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");

    // The linked sources (the only place the text lives).
    const StringView sourcesRoot = u8"scratch_uipipe_src";
    StageSource(sourcesRoot, u8"menu.sml", kUIDocumentStarter);
    WriteText(PathJoin(sourcesRoot, u8"theme.sss").AsView(), kUIThemeStarter);
    foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, foundation::core::DefaultAllocator());

    // Document round-trip.
    auto* docInstance = outDb.RootGroup()->CreateInstance(u8"menu", UIDocumentSource::StaticType());
    {
        UIDocumentAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"menu.sml");
        UIDocumentAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = docInstance;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }
    // Theme round-trip.
    auto* themeInstance = outDb.RootGroup()->CreateInstance(u8"theme", UIThemeSource::StaticType());
    {
        UIThemeAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"theme.sss");
        UIThemeAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = themeInstance;
        REQUIRE(builder.Build(asset, ctx).IsOk());
    }

    UIDocumentFactory documentFactory(DefaultAllocator());
    UIThemeFactory themeFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&documentFactory);
    manager.AddFactory(&themeFactory);
    Proxy<UIDocument> document = manager.Bind<UIDocument>(docInstance->Id());
    REQUIRE(document);
    CHECK(document->markup.AsView() == kUIDocumentStarter);
    Proxy<UITheme> theme = manager.Bind<UITheme>(themeInstance->Id());
    REQUIRE(theme);
    CHECK(!theme->stylesheet.IsEmpty());

    // The cooked markup actually instantiates a view tree with addressable ids.
    MarkupLoader::Initialize();
    RefPtr<View> tree = MarkupLoader::LoadFromString(DefaultAllocator(), document->markup.AsView());
    REQUIRE(tree.Get() != nullptr);
    auto* group = Cast<ViewGroup>(tree.Get());
    REQUIRE(group != nullptr);
    CHECK(group->FindByName(u8"ok-btn") != nullptr);

    RemoveAll(sourcesRoot);
    RemoveTree(u8"scratch_uipipe_db");
}

TEST_CASE("ui.pipeline: malformed payloads FAIL the cook")
{
    RegisterUIResource();
    RegisterUIAssets();
    RemoveTree(u8"scratch_uipipe_bad_db");
    foundation::vfs::NativeFileSystem outMount(u8"scratch_uipipe_bad_db", foundation::core::DefaultAllocator());
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");
    auto* instance = outDb.RootGroup()->CreateInstance(u8"menu", UIDocumentSource::StaticType());

    const StringView sourcesRoot = u8"scratch_uipipe_bad_src";
    StageSource(sourcesRoot, u8"bad.sml", u8"<FlexLayout><Label text=\"unclosed\"</FlexLayout>");
    WriteText(PathJoin(sourcesRoot, u8"unknown.sml").AsView(), u8"<NotARealControl />");
    WriteText(PathJoin(sourcesRoot, u8"empty.sml").AsView(), u8"");
    WriteText(PathJoin(sourcesRoot, u8"empty.sss").AsView(), u8"");
    foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, foundation::core::DefaultAllocator());
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &sourcesMount;
    ctx.output = instance;

    UIDocumentAssetBuilder documents;
    UIDocumentAsset badXml;
    badXml.fileName = foundation::vfs::SourcePath(u8"bad.sml");
    CHECK_FALSE(documents.Build(badXml, ctx).IsOk());
    UIDocumentAsset unknownControl;
    unknownControl.fileName = foundation::vfs::SourcePath(u8"unknown.sml");
    CHECK_FALSE(documents.Build(unknownControl, ctx).IsOk());
    UIDocumentAsset empty;
    empty.fileName = foundation::vfs::SourcePath(u8"empty.sml");
    CHECK_FALSE(documents.Build(empty, ctx).IsOk());
    UIDocumentAsset unlinked; // no fileName: nothing to cook
    CHECK_FALSE(documents.Build(unlinked, ctx).IsOk());

    UIThemeAssetBuilder themes;
    UIThemeAsset emptyTheme;
    emptyTheme.fileName = foundation::vfs::SourcePath(u8"empty.sss");
    CHECK_FALSE(themes.Build(emptyTheme, ctx).IsOk());
    UIThemeAsset unlinkedTheme;
    CHECK_FALSE(themes.Build(unlinkedTheme, ctx).IsOk());

    RemoveAll(sourcesRoot);
    RemoveTree(u8"scratch_uipipe_bad_db");
}

TEST_CASE("ui.pipeline: a gamekit <screen> document validates at cook")
{
    // <screen> is gamekit markup, not a builtin - the cook registers it (RegisterGamekitMarkup)
    // so game HUD/menu roots validate. Without that registration this Build FAILS as an unknown
    // control, exactly like <NotARealControl/> above.
    RegisterUIResource();
    RegisterUIAssets();
    RemoveTree(u8"scratch_uipipe_screen_db");
    foundation::vfs::NativeFileSystem outMount(u8"scratch_uipipe_screen_db", foundation::core::DefaultAllocator());
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");
    auto* instance = outDb.RootGroup()->CreateInstance(u8"hud", UIDocumentSource::StaticType());

    const StringView sourcesRoot = u8"scratch_uipipe_screen_src";
    StageSource(sourcesRoot, u8"hud.sml",
                u8"<screen mode=\"overlay\"><Label id=\"hud-timer\" text=\"90\"/></screen>");
    foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, foundation::core::DefaultAllocator());
    UIDocumentAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"hud.sml");
    UIDocumentAssetBuilder builder;
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &sourcesMount;
    ctx.output = instance;
    CHECK(builder.Build(asset, ctx).IsOk());

    RemoveAll(sourcesRoot);
    RemoveTree(u8"scratch_uipipe_screen_db");
}

TEST_CASE("ui.gamekit: a <screen> child Label keeps its id as Name and is findable")
{
    // Repro for the PaperKid HUD: a <screen mode="overlay"> root with nested id'd Labels. The `ui`
    // facade's ui::findLabel searches the screen root recursively, so the nested Label MUST carry
    // its markup id as its View Name. (The bug this guards: findLabel returning loud-null on a HUD.)
    MarkupLoader::Initialize();
    foundation::ui::gamekit::RegisterGamekitMarkup();
    RefPtr<View> tree = MarkupLoader::LoadFromString(DefaultAllocator(), 
        u8"<screen mode=\"overlay\">"
        u8"  <Panel><Flex><Label id=\"hud-timer\" text=\"90\"/></Flex></Panel>"
        u8"</screen>");
    REQUIRE(tree.Get() != nullptr);
    // The root IS a gamekit UIScreen (used directly, not wrapped).
    CHECK(Cast<foundation::ui::gamekit::UIScreen>(tree.Get()) != nullptr);
    auto* group = Cast<ViewGroup>(tree.Get());
    REQUIRE(group != nullptr);
    // The nested Label is findable by its id, and is a Label (what ui::findLabel casts to).
    CHECK(group->FindByName(u8"hud-timer") != nullptr);
    CHECK(group->FindByName<Label>(u8"hud-timer") != nullptr);
}

TEST_CASE("ui.pipeline: silent markup drops surface as cook warnings")
{
    MarkupLoader::Initialize();
    Array<String> warnings;
    RefPtr<View> tree = MarkupLoader::LoadFromString(DefaultAllocator(), 
        u8"<Flex direction=\"vertical\">"
        u8"  <Label fontSize=\"20\" text=\"typo\"/>" // camelCase typo -> warning
        u8"  <NotARealControl/>"                     // unknown child -> warning (dropped)
        u8"  <Button id=\"ok\" text=\"fine\" height=\"40\"/>"
        u8"</Flex>",
        nullptr, &warnings);
    REQUIRE(tree.Get() != nullptr); // the tree still builds
    REQUIRE(warnings.Size() == 2);
    CHECK(warnings[0].AsView().StartsWith(u8"unknown attribute 'fontSize'"));
    CHECK(warnings[1].AsView().StartsWith(u8"unknown element <NotARealControl>"));

    // A clean document warns about nothing.
    warnings.Clear();
    RefPtr<View> clean = MarkupLoader::LoadFromString(DefaultAllocator(), kUIDocumentStarter, nullptr, &warnings);
    REQUIRE(clean.Get() != nullptr);
    CHECK(warnings.IsEmpty());
}

// The importer stages the dropped .sml/.sss into Sources/ and LINKS it through fileName -
// it does NOT embed the text into the asset (mirrors ScriptFileImporter).
TEST_CASE("ui.pipeline: importer links the dropped file into Sources (no inline text)")
{
    RegisterUIAssets();
    const StringView root = u8"scratch_uipipe_import";
    const String sourcesRoot = PathJoin(root, u8"Sources");
    RemoveAll(root);
    RemoveAll(u8"scratch_uipipe_import_db");
    REQUIRE(CreateDirectories(sourcesRoot.AsView()));

    // Loose OS files to import (living outside the project).
    const String looseDoc = PathJoin(root, u8"panel.sml");
    const String looseTheme = PathJoin(root, u8"skin.sss");
    WriteText(looseDoc.AsView(), kUIDocumentStarter);
    WriteText(looseTheme.AsView(), kUIThemeStarter);

    foundation::vfs::NativeFileSystem outMount(u8"scratch_uipipe_import_db", foundation::core::DefaultAllocator());
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");

    UIFileImporter importer;
    pipeline::ImportContext importCtx{DefaultAllocator(), sourcesRoot.AsView()};

    // Document: fileName SET, source present under Sources/.
    Result<content::Instance*> docInst =
        importer.Import(looseDoc.AsView(), importCtx, *outDb.RootGroup(), nullptr, nullptr, nullptr);
    REQUIRE(docInst.HasValue());
    REQUIRE(docInst.Value() != nullptr);
    {
        RefPtr<ISerializable> object = docInst.Value()->ReadObject();
        auto* asset = Cast<UIDocumentAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName.View() == StringView(u8"panel.sml"));
    }
    CHECK(FileExists(PathJoin(sourcesRoot.AsView(), u8"panel.sml").AsView()));

    // Theme: fileName SET, source present under Sources/.
    Result<content::Instance*> themeInst = importer.Import(looseTheme.AsView(), importCtx,
                                                          *outDb.RootGroup(), nullptr, nullptr,
                                                          nullptr);
    REQUIRE(themeInst.HasValue());
    REQUIRE(themeInst.Value() != nullptr);
    {
        RefPtr<ISerializable> object = themeInst.Value()->ReadObject();
        auto* asset = Cast<UIThemeAsset>(object.Get());
        REQUIRE(asset != nullptr);
        CHECK(asset->fileName.View() == StringView(u8"skin.sss"));
    }
    CHECK(FileExists(PathJoin(sourcesRoot.AsView(), u8"skin.sss").AsView()));

    RemoveAll(root);
    RemoveAll(u8"scratch_uipipe_import_db");
}

// A cook of a LINKED asset reads the text back through the sources mount and the cooked
// product embeds it (only the SOURCE asset links).
TEST_CASE("ui.pipeline: cook of a linked source reads the Sources file into the product")
{
    RegisterUIAssets();
    const StringView sourcesRoot = u8"scratch_uipipe_linked_src";
    RemoveAll(sourcesRoot);
    REQUIRE(CreateDirectories(sourcesRoot));
    WriteText(PathJoin(sourcesRoot, u8"doc.sml").AsView(), kUIDocumentStarter);
    WriteText(PathJoin(sourcesRoot, u8"theme.sss").AsView(), kUIThemeStarter);

    foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, foundation::core::DefaultAllocator());
    RemoveAll(u8"scratch_uipipe_linked_db");
    foundation::vfs::NativeFileSystem outMount(u8"scratch_uipipe_linked_db", foundation::core::DefaultAllocator());
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");

    // Document: fileName points at doc.sml -> cooked markup equals the file text.
    auto* docInst = outDb.RootGroup()->CreateInstance(u8"doc", UIDocumentSource::StaticType());
    {
        UIDocumentAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"doc.sml");
        UIDocumentAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = docInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
        RefPtr<ISerializable> object = docInst->ReadObject();
        auto* cooked = Cast<UIDocumentSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->markup.AsView() == kUIDocumentStarter);
    }
    // Theme: fileName points at theme.sss -> cooked stylesheet equals the file text.
    auto* themeInst = outDb.RootGroup()->CreateInstance(u8"theme", UIThemeSource::StaticType());
    {
        UIThemeAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"theme.sss");
        UIThemeAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = themeInst;
        REQUIRE(builder.Build(asset, ctx).IsOk());
        RefPtr<ISerializable> object = themeInst->ReadObject();
        auto* cooked = Cast<UIThemeSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->stylesheet.AsView() == kUIThemeStarter);
    }

    // A linked asset whose source file is MISSING fails the cook (not an empty product).
    {
        UIDocumentAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"does-not-exist.sml");
        UIDocumentAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = docInst;
        CHECK_FALSE(builder.Build(asset, ctx).IsOk());
    }

    RemoveAll(sourcesRoot);
    RemoveAll(u8"scratch_uipipe_linked_db");
}

TEST_CASE("ui.pipeline: theme previewMarkup round-trips on the SOURCE asset but never reaches the cook")
{
    // messaging: the sss editor stores its preview markup on the theme (cross-session), but that is
    // EDITOR-ONLY (no-editor-data-in-runtime): it persists in the source asset's Serialize and is
    // DELIBERATELY excluded from the cooked UIThemeSource (structurally - no such field).
    RegisterUIResource();
    RegisterUIAssets();
    RemoveDirectory(u8"scratch_uipipe_preview_db");
    foundation::vfs::NativeFileSystem mount(u8"scratch_uipipe_preview_db", foundation::core::DefaultAllocator());
    content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
    const StringView sourcesRoot = u8"scratch_uipipe_preview_src";
    StageSource(sourcesRoot, u8"theme.sss", kUIThemeStarter);
    foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, foundation::core::DefaultAllocator());

    // SOURCE round-trip: previewMarkup survives WriteObject -> ReadObject.
    auto* src = db.RootGroup()->CreateInstance(u8"theme_src", UIThemeAsset::StaticType());
    {
        UIThemeAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"theme.sss");
        asset.previewMarkup = String(u8"<Panel><Button text=\"Preview\"/></Panel>");
        REQUIRE(src->WriteObject(asset).IsOk());
    }
    {
        RefPtr<ISerializable> object = src->ReadObject();
        auto* loaded = Cast<UIThemeAsset>(object.Get());
        REQUIRE(loaded != nullptr);
        CHECK(loaded->previewMarkup.AsView() == u8"<Panel><Button text=\"Preview\"/></Panel>");
        CHECK(loaded->fileName.View() == StringView(u8"theme.sss")); // the link is unaffected
    }

    // COOK: the product is a UIThemeSource carrying ONLY the stylesheet - previewMarkup cannot leak
    // (the runtime struct has no field for it), and the builder never reads it.
    auto* product = db.RootGroup()->CreateInstance(u8"theme_cooked", UIThemeSource::StaticType());
    {
        UIThemeAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"theme.sss");
        asset.previewMarkup = String(u8"<Panel/>");
        UIThemeAssetBuilder builder;
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = product;
        REQUIRE(builder.Build(asset, ctx).IsOk());
        RefPtr<ISerializable> object = product->ReadObject();
        auto* cooked = Cast<UIThemeSource>(object.Get());
        REQUIRE(cooked != nullptr);
        CHECK(cooked->stylesheet.AsView() == kUIThemeStarter);
    }

    RemoveAll(sourcesRoot);
    RemoveDirectory(u8"scratch_uipipe_preview_db");
}

// ---- vector images: an SVG asset, and a theme that draws it as an icon ----

namespace
{
    constexpr StringView kHeartSvg =
        u8"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\">"
        u8"<path d=\"M12 21 L3 12 A5 5 0 0 1 12 5 A5 5 0 0 1 21 12 Z\" fill=\"#E53935\"/></svg>";
}

TEST_CASE("ui.pipeline: the UI importer takes documents and themes; an .svg is the vector image importer's")
{
    UIFileImporter importer;
    CHECK(importer.Accepts(u8"sml"));
    CHECK(importer.Accepts(u8"sss"));
    CHECK_FALSE(importer.Accepts(u8"svg"));
}

TEST_CASE("ui.pipeline: a theme's @icon embeds the vector image it names, and an unknown one fails")
{
    RegisterUIResource();
    RegisterUIAssets();
    foundation::vg::RegisterVGResource();
    RegisterVGAssets();
    RemoveAll(u8"scratch_uipipe_icon_db");
    REQUIRE(CreateDirectories(u8"scratch_uipipe_icon_db"));
    foundation::vfs::NativeFileSystem outMount(u8"scratch_uipipe_icon_db", DefaultAllocator());
    content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");
    const StringView sourcesRoot = u8"scratch_uipipe_icon_src";
    StageSource(sourcesRoot, u8"heart.svg", kHeartSvg);
    foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, DefaultAllocator());

    // The vector image, cooked first (the theme's `reads` edge orders it so in a real cook).
    auto* heart = outDb.RootGroup()->CreateInstance(u8"heart", foundation::vg::VectorImageSource::StaticType());
    {
        VectorImageAsset asset;
        asset.fileName = foundation::vfs::SourcePath(u8"heart.svg");
        pipeline::AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = heart;
        REQUIRE(VectorImageAssetBuilder{}.Build(asset, ctx).IsOk());
    }
    const String reference = Format(u8"{{{}}}", heart->Id());
    String sheet(u8"@icon heart \"");
    sheet.Append(reference.AsView());
    sheet.Append(u8"\";\n.lives-icon { background: svg(heart, tint=#E53935); }\n");
    WriteText(PathJoin(sourcesRoot, u8"theme.sss").AsView(), sheet.AsView());

    UIThemeAsset asset;
    asset.fileName = foundation::vfs::SourcePath(u8"theme.sss");
    UIThemeAssetBuilder builder;
    pipeline::AssetBuildContext ctx{DefaultAllocator()};
    ctx.sources = &sourcesMount;
    ctx.db = &outDb;

    // The scan names the image as content the theme reads, so a changed SVG recooks the theme.
    pipeline::AssetDependencies dependencies;
    builder.ScanDependencies(asset, ctx, dependencies);
    REQUIRE(dependencies.reads.Size() == 1u);
    CHECK(dependencies.reads[0] == heart->Id());

    auto* themeInstance = outDb.RootGroup()->CreateInstance(u8"theme", UIThemeSource::StaticType());
    ctx.output = themeInstance;
    REQUIRE(builder.Build(asset, ctx).IsOk());
    UIThemeFactory themeFactory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), outDb);
    manager.AddFactory(&themeFactory);
    Proxy<UITheme> theme = manager.Bind<UITheme>(themeInstance->Id());
    REQUIRE(theme);
    REQUIRE(theme->icons.Size() == 1u);
    const String* svg = theme->FindIcon(reference.AsView());
    REQUIRE(svg != nullptr);
    CHECK(svg->AsView() == kHeartSvg);

    // An @icon naming no vector image fails the cook rather than drawing nothing.
    WriteText(PathJoin(sourcesRoot, u8"theme.sss").AsView(),
              u8"@icon star \"{6dd1ae0e-fbe8-4c9b-8c9e-d10b727f4d84}\";\n.star { background: svg(star); }\n");
    ctx.output = outDb.RootGroup()->CreateInstance(u8"theme2", UIThemeSource::StaticType());
    CHECK_FALSE(builder.Build(asset, ctx).IsOk());

    RemoveAll(sourcesRoot);
    RemoveAll(u8"scratch_uipipe_icon_db");
}
