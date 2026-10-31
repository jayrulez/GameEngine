// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The script page over its markup without a host (AngelScript). The source is read into the code
// and checked; a broken edit reports a problem on its line; there is one history (the code's
// steps go on the page's stack, and Ctrl+Z is the page's undo); Save writes the source; a missing
// source is a problem and cannot be saved; the API browser is built when opened and can be
// searched; and a breakpoint belongs to the editor.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.script;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.gui.code;
import script.pipeline;
#ifdef OPTION_HAS_ANGELSCRIPT
import script.angelscript.pipeline;
#endif
import editor.core;
import editor.app;
import editor.script;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

#ifdef OPTION_HAS_ANGELSCRIPT
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

    // A project with a behaviour made from the cook's starter ("Hero", Sources/Hero.as) and one
    // whose source is missing ("Gone").
    struct Bed
    {
        String dir = String(u8"scratch_script_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        app::CodeLanguages languages{DefaultAllocator(), [](StringView path, String& out) { return ReadData(path, out); }};
        foundation::content::Instance* hero = nullptr;
        foundation::content::Instance* gone = nullptr;
        String source;

        Bed()
        {
            pipeline::RegisterScriptAssets();
            pipeline::RegisterAngelScriptScriptCook();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            pipeline::IScriptLanguageCook* cook = pipeline::ScriptLanguageCookRegistry::Get().FindByLanguage(u8"angelscript");
            REQUIRE(cook != nullptr);
            source = cook->NewAssetTemplate(pipeline::ScriptTier::Behavior, u8"Hero");
            REQUIRE(WriteFile(PathJoin(project->SourcesRoot().AsView(), u8"Hero.as").AsView(),
                              Span<const byte>(reinterpret_cast<const byte*>(source.Data()), source.Size())).IsOk());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            hero = top->CreateInstance(u8"Hero", pipeline::ScriptClassAsset::StaticType());
            gone = top->CreateInstance(u8"Gone", pipeline::ScriptClassAsset::StaticType());
            REQUIRE(hero != nullptr);
            REQUIRE(gone != nullptr);
            pipeline::ScriptClassAsset asset;
            asset.language = String(u8"angelscript");
            asset.fileName = foundation::vfs::SourcePath(u8"Hero.as");
            REQUIRE(hero->WriteObject(asset).IsOk());
            asset.fileName = foundation::vfs::SourcePath(u8"Gone.as");
            REQUIRE(gone->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<ScriptEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            services.languages = &languages;
            return MakeUnique<ScriptEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
        String OnDisk() const
        {
            String text;
            Result<Array<byte>> bytes = ReadFile(PathJoin(project->SourcesRoot().AsView(), u8"Hero.as").AsView());
            if (bytes.HasValue())
                text = String(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
            return text;
        }
    };
}

TEST_CASE("script page: registered for the script asset")
{
    const app::AssetPageServicesSource none;
    EditorContext context{DefaultAllocator()};
    RegisterScriptEditor(context, none);
    IEditorPageFactory* found = context.Pages().FindFactory(pipeline::ScriptClassAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::ScriptClassAsset::StaticType());
}

TEST_CASE("script page: the source read and checked, a broken edit's problem on its line, one history, Save writing it")
{
    Bed bed;
    UniquePtr<ScriptEditorPage> page = bed.Open(*bed.hero);
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Code() != nullptr);
    REQUIRE(page->Problems() != nullptr);
    CHECK(page->SourceRead());
    CHECK(page->Text() == bed.source);
    CHECK(page->Language() == StringView(u8"angelscript"));
    CHECK(page->ErrorCount() == 0u);
    CHECK(page->Problems()->Summary() == StringView(u8"No problems: compiles class Hero")); // names the class it compiled
    CHECK_FALSE(page->Code()->TokensOf(0).IsEmpty()); // coloured, so its language was found

    // Typing that breaks the code is one step on the page's stack, and once checked the problem
    // shows on its line.
    page->Code()->SetCaret(gui::CodePosition{0, 0});
    page->Code()->Type(u8"this does not compile ");
    CHECK(page->IsDirty());
    CHECK(page->Commands().CanUndo());
    page->CheckNow();
    REQUIRE(page->ErrorCount() > 0u);
    CHECK(page->Found()[0].line == 0u);
    CHECK_FALSE(page->Code()->GetDiagnostics().IsEmpty());

    // The page's undo walks the code's history; redo puts it back.
    page->Commands().Undo();
    CHECK(page->Text() == bed.source);
    CHECK_FALSE(page->Commands().CanUndo());
    page->Commands().Redo();
    CHECK(page->Text().AsView().StartsWith(u8"this does not compile "));
    page->Commands().Undo();

    // An edit, once saved, is in the file.
    page->Code()->SetCaret(gui::CodePosition{0, 0});
    page->Code()->Type(u8"// a note\n");
    REQUIRE(page->Save().IsOk());
    CHECK_FALSE(page->IsDirty());
    CHECK(bed.OnDisk().AsView().StartsWith(u8"// a note\n"));
    CHECK(page->ErrorCount() == 0u);
}

TEST_CASE("script page: a source that is not there is a problem, the code read only, Save refused")
{
    Bed bed;
    UniquePtr<ScriptEditorPage> page = bed.Open(*bed.gone);
    CHECK_FALSE(page->SourceRead());
    REQUIRE(page->Code() != nullptr);
    CHECK(page->Code()->IsReadOnly());
    REQUIRE(page->Found().Size() == 1u);
    CHECK(page->Found()[0].message.AsView().ContainsIgnoreCase(u8"Gone.as"));
    CHECK_FALSE(page->Save().IsOk());
}

TEST_CASE("script page: the API browser built when it opens and searched; a breakpoint the editor's")
{
    Bed bed;
    UniquePtr<ScriptEditorPage> page = bed.Open(*bed.hero);
    CHECK_FALSE(page->ApiShown());
    CHECK(page->ApiTree().nodes.IsEmpty()); // not built while closed
    page->ShowApi(true);
    CHECK(page->ApiShown());
    CHECK_FALSE(page->ApiTree().roots.IsEmpty());
    REQUIRE(page->ApiModel() != nullptr); // the tree view's model, not only the page's list of rows
    CHECK(page->ApiModel()->RowCount() == page->ApiTree().roots.Size());
    page->SearchApi(u8"Float3");
    bool float3 = false;
    for (const i32 root : page->ApiTree().roots)
        float3 = float3 || page->ApiTree().nodes[static_cast<usize>(root)].label.AsView() == StringView(u8"Float3");
    CHECK(float3);
    page->ShowApi(false);
    CHECK_FALSE(page->ApiShown());

    page->ToggleBreakpoint(2);
    CHECK(page->HasBreakpointMark(2));
    CHECK(bed.context.HasBreakpoint(u8"Hero.as", 3));
    page->ToggleBreakpoint(2);
    CHECK_FALSE(page->HasBreakpointMark(2));
    CHECK_FALSE(bed.context.HasBreakpoint(u8"Hero.as", 3));
}
#endif // OPTION_HAS_ANGELSCRIPT
