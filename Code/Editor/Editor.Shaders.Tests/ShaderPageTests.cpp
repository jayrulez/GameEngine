// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The shader page over its markup without a host (no preview drawn): the source read and checked
// against the engine's library (what the shader is, its properties), a broken edit's problem with
// the preview kept on the last version that compiled and saying so, and one history for the code
// and the asset's grid. Skipped where no shader compiler is available, as the pipeline's tests are.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.shaders;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.gui.code;
import shaders.pipeline;
import editor.core;
import editor.app;
import editor.shaders;

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
    bool HaveCompiler()
    {
        foundation::shaders::Compiler* compiler = nullptr;
        if (!foundation::shaders::createCompiler(foundation::shaders::CompilerDesc{}, compiler).IsOk() || compiler == nullptr)
            return false;
        compiler->Destroy();
        return true;
    }

    // A project with the gallery's first lesson ("Colour", Sources/Colour.hlsl: an unlit surface
    // shader with one property).
    struct Bed
    {
        String dir = String(u8"scratch_shader_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        app::CodeLanguages languages{DefaultAllocator(), [](StringView path, String& out) { return ReadData(path, out); }};
        foundation::content::Instance* colour = nullptr;
        String source;

        Bed()
        {
            pipeline::RegisterShaderAsset();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            REQUIRE(ReadData(u8"SampleProjects/ShaderGallery/Sources/01_UnlitColour.hlsl", source));
            // Line feeds alone, as the page's code document holds a file's text: a Windows checkout
            // gives the sample CRLF endings.
            String feeds;
            for (const char8_t c : source.AsView())
                if (c != u8'\r')
                    feeds.Append(StringView(&c, 1));
            source = Move(feeds);
            REQUIRE(WriteFile(PathJoin(project->SourcesRoot().AsView(), u8"Colour.hlsl").AsView(),
                              Span<const byte>(reinterpret_cast<const byte*>(source.Data()), source.Size())).IsOk());
            colour = project->SourceDb().RootGroup()->CreateInstance(u8"Colour", pipeline::ShaderAsset::StaticType());
            REQUIRE(colour != nullptr);
            pipeline::ShaderAsset asset;
            asset.name = String(u8"Colour");
            asset.fileName = foundation::vfs::SourcePath(u8"Colour.hlsl");
            REQUIRE(colour->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<ShaderEditorPage> Open()
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            services.languages = &languages;
            return MakeUnique<ShaderEditorPage>(DefaultAllocator(), context, Move(services), *colour, DataPath(u8"Engine/Shaders").AsView());
        }
    };
}

TEST_CASE("shader page: registered for the shader asset")
{
    const app::AssetPageServicesSource none;
    EditorContext context{DefaultAllocator()};
    RegisterShaderEditor(context, none, u8"Engine/Shaders");
    IEditorPageFactory* found = context.Pages().FindFactory(pipeline::ShaderAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::ShaderAsset::StaticType());
}

TEST_CASE("shader page: checked against the engine's library, a broken edit's problem with the preview kept, one history with the grid")
{
    if (!HaveCompiler())
    {
        MESSAGE("no shader compiler; skipping");
        return;
    }
    Bed bed;
    UniquePtr<ShaderEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Code() != nullptr);
    CHECK(page->Text() == bed.source);
    CHECK(page->ErrorCount() == 0u);
    CHECK(page->Kind() == StringView(u8"Unlit surface shader"));
    CHECK(page->PropertyCount() == 1u);
    CHECK_FALSE(page->PreviewStale());

    // Broken: its problem, and the preview kept on the last version that compiled.
    const u32 last = page->Code()->GetDocument().LineCount() - 1;
    page->Code()->SetCaret(gui::CodePosition{last, 0});
    page->Code()->Type(u8"this is not hlsl\n");
    page->CheckNow();
    CHECK(page->ErrorCount() > 0u);
    CHECK(page->Kind().IsEmpty());
    CHECK(page->PreviewStale());

    // The grid's edit after the typing: one history, undone in order.
    page->Edit(u8"name", gui::PropertyWrite{gui::Variant(StringView(u8"Paint"))});
    auto* asset = Cast<pipeline::ShaderAsset>(page->Object());
    REQUIRE(asset != nullptr);
    CHECK(asset->name == StringView(u8"Paint"));
    page->Commands().Undo(); // the name
    CHECK(asset->name == StringView(u8"Colour"));
    CHECK(page->Text() != bed.source);
    page->Commands().Undo(); // the typing
    CHECK(page->Text() == bed.source);
    page->CheckNow();
    CHECK(page->ErrorCount() == 0u);
    CHECK_FALSE(page->PreviewStale());
}
