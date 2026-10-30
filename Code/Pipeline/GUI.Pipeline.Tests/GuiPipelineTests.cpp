// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Pipeline (E3 of Specs/gui-lab-evolution-3.md): a `.xml` document and a `.css` sheet cooked
// through their validating builders and loaded as products; what cannot be read fails the cook, what
// was dropped is said with its line; the guids a source names are found; the importer links a
// dropped file as the asset its extension says.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include <filesystem>
#include <initializer_list>

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import pipeline.core;
import pipeline.importer;
import foundation.gui;
import foundation.gui.resources;
import gui.pipeline;

using namespace foundation::core;
using namespace foundation::resource;
using namespace pipeline;
namespace content = foundation::content;
namespace gui = foundation::gui;

namespace
{
    void WriteText(StringView path, StringView text)
    {
        REQUIRE(WriteFile(path, Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }
    void RemoveAll(StringView dir)
    {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(reinterpret_cast<const char*>(String(dir).CStr())), ec);
    }
    bool Says(const Array<gui::GuiDiagnostic>& all, StringView part)
    {
        for (const gui::GuiDiagnostic& d : all)
            if (gui::MarkupLoader::FindFrom(d.Message.AsView(), part, 0) < d.Message.Size())
                return true;
        return false;
    }

    constexpr StringView kTitle = u8"<Screen covers=\"true\" nav-default=\"play\">\n"
                                  u8"  <Button id=\"play\" text=\"@menu.play\"/>\n"
                                  u8"  <Prompt action=\"pause\"/>\n"
                                  u8"</Screen>\n";
    constexpr StringView kSheet = u8"screen:entering { opacity: 0; }\n"
                                  u8"button { background-image: url(\"{6b1c0d3e-0000-4000-8000-000000000001}\"); }\n";
}

TEST_CASE("gui.pipeline: a document and a sheet cook (validated) and load as their products")
{
    gui::RegisterGuiResource();
    RegisterGuiAssets();
    RemoveAll(u8"scratch_guipipe_db");
    REQUIRE(CreateDirectories(u8"scratch_guipipe_db"));
    const StringView sourcesRoot = u8"scratch_guipipe_src";
    RemoveAll(sourcesRoot);
    REQUIRE(CreateDirectories(sourcesRoot));
    WriteText(PathJoin(sourcesRoot, u8"title.xml").AsView(), kTitle);
    WriteText(PathJoin(sourcesRoot, u8"game.css").AsView(), kSheet);
    {
        foundation::vfs::NativeFileSystem outMount(u8"scratch_guipipe_db", DefaultAllocator());
        content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");
        foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, DefaultAllocator());

        auto* documentInstance = outDb.RootGroup()->CreateInstance(u8"title", gui::GuiDocumentSource::StaticType());
        auto* sheetInstance = outDb.RootGroup()->CreateInstance(u8"game", gui::GuiStyleSheetSource::StaticType());
        {
            GuiDocumentAsset asset;
            asset.fileName = foundation::vfs::SourcePath(u8"title.xml");
            GuiDocumentAssetBuilder builder;
            AssetBuildContext ctx{DefaultAllocator()};
            ctx.sources = &sourcesMount;
            ctx.output = documentInstance;
            REQUIRE(builder.Build(asset, ctx).IsOk());
        }
        {
            GuiStyleSheetAsset asset;
            asset.fileName = foundation::vfs::SourcePath(u8"game.css");
            GuiStyleSheetAssetBuilder builder;
            AssetBuildContext ctx{DefaultAllocator()};
            ctx.sources = &sourcesMount;
            ctx.output = sheetInstance;
            REQUIRE(builder.Build(asset, ctx).IsOk());
        }
        gui::GuiDocumentFactory documents(DefaultAllocator());
        gui::GuiStyleSheetFactory sheets(DefaultAllocator());
        ResourceManager manager(DefaultAllocator(), outDb);
        manager.AddFactory(&documents);
        manager.AddFactory(&sheets);
        Proxy<gui::GuiDocument> document = manager.Bind<gui::GuiDocument>(documentInstance->Id());
        Proxy<gui::GuiStyleSheet> sheet = manager.Bind<gui::GuiStyleSheet>(sheetInstance->Id());
        REQUIRE(document);
        REQUIRE(sheet);
        CHECK(document->markup.AsView() == kTitle);
        CHECK(sheet->css.AsView() == kSheet);
    }
    RemoveAll(sourcesRoot);
    RemoveAll(u8"scratch_guipipe_db");
}

TEST_CASE("gui.pipeline: markup that cannot be read fails the cook; what was dropped is said, with its line")
{
    Array<gui::GuiDiagnostic> said;
    CHECK(ValidateGuiDocument(kTitle, said, DefaultAllocator())); // the gamekit's Screen and Prompt are known
    CHECK(said.IsEmpty());

    said.Clear();
    CHECK_FALSE(ValidateGuiDocument(u8"<Panel>\n  <Label text=\"unclosed\"\n</Panel>", said, DefaultAllocator()));
    CHECK(Says(said, u8"does not parse"));
    said.Clear();
    CHECK_FALSE(ValidateGuiDocument(u8"<NotAnElement/>", said, DefaultAllocator())); // nothing to make
    said.Clear();
    CHECK_FALSE(ValidateGuiDocument(u8"", said, DefaultAllocator()));
    said.Clear();
    CHECK(ValidateGuiDocument(u8"<Panel><Lable/><Label txet=\"x\"/></Panel>", said, DefaultAllocator())); // read, with what it dropped
    CHECK(Says(said, u8"<Lable>"));
    CHECK(Says(said, u8"'txet'"));

    said.Clear();
    ValidateGuiStyleSheet(u8".a { colour: red; }\n.b:hovr { color: blue; }\nscreen { covers: true; }\n", said);
    CHECK(Says(said, u8"'colour'"));
    CHECK(Says(said, u8".b:hovr"));
    CHECK_FALSE(Says(said, u8"covers")); // the gamekit's Screen takes it
    for (const gui::GuiDiagnostic& d : said)
        CHECK(d.Line != 0u);

    // The builder: no link, or an unreadable document, fails.
    RemoveAll(u8"scratch_guipipe_bad_db");
    REQUIRE(CreateDirectories(u8"scratch_guipipe_bad_db"));
    const StringView sourcesRoot = u8"scratch_guipipe_bad_src";
    RemoveAll(sourcesRoot);
    REQUIRE(CreateDirectories(sourcesRoot));
    WriteText(PathJoin(sourcesRoot, u8"bad.xml").AsView(), u8"<Panel><Label text=\"x\"</Panel>");
    {
        foundation::vfs::NativeFileSystem outMount(u8"scratch_guipipe_bad_db", DefaultAllocator());
        content::ContentDatabase outDb(DefaultAllocator(), outMount, BinarySerializerFactory(), u8".rasset");
        foundation::vfs::NativeFileSystem sourcesMount(sourcesRoot, DefaultAllocator());
        AssetBuildContext ctx{DefaultAllocator()};
        ctx.sources = &sourcesMount;
        ctx.output = outDb.RootGroup()->CreateInstance(u8"bad", gui::GuiDocumentSource::StaticType());
        GuiDocumentAssetBuilder builder;
        GuiDocumentAsset bad;
        bad.fileName = foundation::vfs::SourcePath(u8"bad.xml");
        CHECK_FALSE(builder.Build(bad, ctx).IsOk());
        GuiDocumentAsset unlinked;
        CHECK_FALSE(builder.Build(unlinked, ctx).IsOk());
        GuiDocumentAsset missing;
        missing.fileName = foundation::vfs::SourcePath(u8"missing.xml");
        CHECK_FALSE(builder.Build(missing, ctx).IsOk());
    }
    RemoveAll(sourcesRoot);
    RemoveAll(u8"scratch_guipipe_bad_db");
}

TEST_CASE("gui.pipeline: the guids a source names are found, each once")
{
    const Array<Guid> found = FindGuidReferences(u8"url(\"{6b1c0d3e-0000-4000-8000-000000000001}\") "
                                                 u8"source=\"{6b1c0d3e-0000-4000-8000-000000000002}\" "
                                                 u8"again {6b1c0d3e-0000-4000-8000-000000000001} not {a-guid} nor {}");
    REQUIRE(found.Size() == 2);
    CHECK(found[0] == Guid(StringView(u8"6b1c0d3e-0000-4000-8000-000000000001")));
    CHECK(found[1] == Guid(StringView(u8"6b1c0d3e-0000-4000-8000-000000000002")));
}

TEST_CASE("gui.pipeline: the importer takes .xml and .css; the creators make a document and a sheet")
{
    GuiFileImporter importer;
    CHECK(importer.Accepts(u8"xml"));
    CHECK(importer.Accepts(u8"css"));
    CHECK_FALSE(importer.Accepts(u8"gml")); // markup's old extension
    CHECK_FALSE(importer.Accepts(u8"sml"));
    AssetCreatorRegistry creators{DefaultAllocator()};
    RegisterGuiCreators(creators);
    CHECK(creators.Count() == 2u);
    // The starters cook clean.
    Array<gui::GuiDiagnostic> said;
    CHECK(ValidateGuiDocument(kGuiDocumentStarter, said, DefaultAllocator()));
    ValidateGuiStyleSheet(kGuiStyleSheetStarter, said);
    CHECK(said.IsEmpty());
}

// The import's name clash rule, through the GUI importer (a document and a sheet share a stem
// easily): a sheet over a document of the same name is refused, the document untouched;
// re-importing the document's own file replaces it in place (its guid kept); another folder's
// file of the same name is refused, kept beside ("menu.2", its own copy "menu.2.xml"), or takes
// the asset over (guid kept), as the context says; and the window's check (FindClash) says so first.
TEST_CASE("gui.pipeline: an import's name clashes: another type refused; another file refused, kept beside or replacing")
{
    RegisterGuiAssets();
    const String root = PathJoin(GetCurrentDirectory().AsView(), u8"scratch_guipipe_clash");
    RemoveAll(root.AsView());
    const String sources = PathJoin(root.AsView(), u8"Sources");
    const String one = PathJoin(root.AsView(), u8"one");
    const String two = PathJoin(root.AsView(), u8"two");
    const String db = PathJoin(root.AsView(), u8"db");
    for (const String* d : {&sources, &one, &two, &db})
        REQUIRE(CreateDirectories(d->AsView()));
    const String doc = PathJoin(one.AsView(), u8"menu.xml");
    const String sheet = PathJoin(one.AsView(), u8"menu.css");
    const String otherDoc = PathJoin(two.AsView(), u8"menu.xml");
    WriteText(doc.AsView(), kTitle);
    WriteText(sheet.AsView(), kSheet);
    WriteText(otherDoc.AsView(), u8"<Screen covers=\"true\"/>\n");

    foundation::vfs::NativeFileSystem mount(db.AsView(), DefaultAllocator());
    content::ContentDatabase sourceDb(DefaultAllocator(), mount, BinarySerializerFactory(), u8".asset");
    content::Group& group = *sourceDb.RootGroup();
    GuiFileImporter guiImporter;
    IFileImporter& importer = guiImporter; // the interface's defaults (no options, no deferred writes)
    ImportContext context{DefaultAllocator(), sources.AsView()};
    context.ledgerPath = ImportLedgerPath(PathJoin(root.AsView(), u8".cache").AsView());
    auto fileOf = [](content::Instance& instance)
    {
        RefPtr<ISerializable> object = instance.ReadObject();
        return String(Cast<Asset>(object.Get())->fileName.View());
    };

    Result<content::Instance*> first = importer.Import(doc.AsView(), context, group);
    REQUIRE(first.HasValue());
    const Guid id = first.Value()->Id();
    CHECK(first.Value()->TypeName() == StringView(u8"GuiDocumentAsset"));

    // A sheet named "menu": another type holds it. Said first, refused, nothing changed.
    const ImportClash typeClash = importer.FindClash(group, sheet.AsView(), SingleAssetPlan(sheet.AsView()), context.ledgerPath.AsView());
    CHECK(typeClash.kind == ImportClashKind::OtherType);
    CHECK(DescribeImportClash(typeClash).AsView().ContainsIgnoreCase(u8"GuiDocumentAsset"));
    context.onConflict = ImportConflictChoice::Replace; // no choice lets another type through
    Result<content::Instance*> overType = importer.Import(sheet.AsView(), context, group);
    CHECK_FALSE(overType.HasValue());
    CHECK(group.GetInstance(u8"menu")->TypeName() == StringView(u8"GuiDocumentAsset"));
    // Under another name it comes in.
    RefPtr<ImportOptions> named = MakeRef<ImportOptions>(DefaultAllocator());
    named->selection = SingleAssetPlan(sheet.AsView());
    named->selection.entries[0].targetName = String(u8"menu sheet");
    REQUIRE(importer.Import(sheet.AsView(), context, group, named.Get()).HasValue());
    context.onConflict = ImportConflictChoice::Refuse;

    // The document's own file again (edited): a re-import, in place, no question.
    WriteText(doc.AsView(), u8"<Screen covers=\"false\"/>\n");
    CHECK(importer.FindClash(group, doc.AsView(), {}, context.ledgerPath.AsView()).kind == ImportClashKind::Reimport);
    Result<content::Instance*> again = importer.Import(doc.AsView(), context, group);
    REQUIRE(again.HasValue());
    CHECK(again.Value()->Id() == id);
    CHECK(fileOf(*again.Value()) == StringView(u8"menu.xml"));

    // Another folder's menu.xml: refused by default.
    CHECK(importer.FindClash(group, otherDoc.AsView(), {}, context.ledgerPath.AsView()).kind == ImportClashKind::SameType);
    CHECK_FALSE(importer.Import(otherDoc.AsView(), context, group).HasValue());
    CHECK(group.GetInstance(u8"menu.2") == nullptr);
    // Kept beside: its own asset and its own copy; the first's untouched.
    context.onConflict = ImportConflictChoice::KeepBoth;
    Result<content::Instance*> beside = importer.Import(otherDoc.AsView(), context, group);
    REQUIRE(beside.HasValue());
    CHECK(beside.Value()->Name() == StringView(u8"menu.2"));
    CHECK(beside.Value()->Id() != id);
    CHECK(fileOf(*beside.Value()) == StringView(u8"menu.2.xml"));
    CHECK(fileOf(*group.GetInstance(u8"menu")) == StringView(u8"menu.xml"));
    // Replacing: the first asset takes the other file, its guid kept.
    context.onConflict = ImportConflictChoice::Replace;
    Result<content::Instance*> replaced = importer.Import(otherDoc.AsView(), context, group);
    REQUIRE(replaced.HasValue());
    CHECK(replaced.Value()->Id() == id);
    CHECK(fileOf(*replaced.Value()) == StringView(u8"menu.2.xml")); // the other file's copy, the first's left alone
    RemoveAll(root.AsView());
}
