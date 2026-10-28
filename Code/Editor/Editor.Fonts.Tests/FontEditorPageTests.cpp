// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// FontEditorPage tests: the page from the image pages' markup over a font in a project (Roboto, the
// bundled face): its atlas baked in place (no job service here) and shown with its facts, the
// raster ramp's sizes a row of the page's own (text that is no list refused with a notice, a list
// one undo step), shown only while the font bakes a ramp.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.vfs;
import foundation.image;
import fonts.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.fonts;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    // A project holding Roboto ("roboto.ttf") under a font ("Roboto") baking ASCII at 12 and 16.
    struct Bed
    {
        String dir = String(u8"scratch_font_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* roboto = nullptr;

        Bed()
        {
            pipeline::RegisterFontAsset(); // read back by its type, as the editor registers it
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            const String face = PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data/Engine/Fonts/roboto/Roboto-Regular.ttf");
            Result<Array<byte>> bytes = ReadFile(face.AsView());
            REQUIRE(bytes.HasValue());
            REQUIRE(WriteFile(PathJoin(project->SourcesRoot().AsView(), u8"roboto.ttf").AsView(), bytes.Value().AsSpan()).IsOk());
            roboto = project->SourceDb().RootGroup()->CreateInstance(u8"Roboto", pipeline::FontAsset::StaticType());
            REQUIRE(roboto != nullptr);
            pipeline::FontAsset asset;
            asset.fileName = foundation::vfs::SourcePath(u8"roboto.ttf");
            asset.mode = pipeline::FontBakeMode::RasterRamp;
            asset.sizes.Clear();
            asset.sizes.PushBack(12.0f);
            asset.sizes.PushBack(16.0f);
            asset.firstCodepoint = 32;
            asset.lastCodepoint = 126;
            REQUIRE(roboto->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        app::PickerRequest picked; // what the page last asked the picker to show
        UniquePtr<FontEditorPage> Open()
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            services.pick = [this](app::PickerRequest request) { picked = Move(request); };
            return MakeUnique<FontEditorPage>(DefaultAllocator(), context, Move(services), *roboto);
        }
    };
}

TEST_CASE("FontEditor registers a factory the registry routes for FontAsset")
{
    const app::AssetPageServicesSource none; // no services: the registration only, before the context it outlives
    EditorContext context{DefaultAllocator()};
    RegisterFontEditor(context, none);
    IEditorPageFactory* found = context.Pages().FindFactory(pipeline::FontAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::FontAsset::StaticType());
}

TEST_CASE("font page: its atlas baked and shown with its facts; the ramp's sizes a row of its own, one undo step, text that is no list refused")
{
    Bed bed;
    UniquePtr<FontEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Preview()->View().HasImage());
    CHECK(page->PreviewGlyphs() > 50u);
    CHECK(page->SourceFile() == StringView(u8"roboto.ttf"));
    CHECK_FALSE(page->Fact(u8"Atlas").IsEmpty());
    CHECK(page->Ramp().GetValue(u8"sizes", 0).AsString() == StringView(u8"12, 16"));

    auto* asset = Cast<pipeline::FontAsset>(page->Object());
    REQUIRE(asset != nullptr);
    CHECK_FALSE(page->SetSizes(u8"12, big"));
    CHECK_FALSE(page->Notice(u8"sizes").IsEmpty());
    CHECK(page->Ramp().GetValue(u8"sizes", 0).AsString() == StringView(u8"12, 16"));
    CHECK_FALSE(page->Commands().CanUndo());

    REQUIRE(page->SetSizes(u8"10 20.5"));
    CHECK(page->Notice(u8"sizes").IsEmpty());
    REQUIRE(asset->sizes.Size() == 2u);
    CHECK(asset->sizes[1] == doctest::Approx(20.5f));
    CHECK(page->Ramp().GetValue(u8"sizes", 0).AsString() == StringView(u8"10, 20.5"));
    page->Commands().Undo();
    REQUIRE(asset->sizes.Size() == 2u);
    CHECK(asset->sizes[1] == doctest::Approx(16.0f));
    CHECK(page->Ramp().GetValue(u8"sizes", 0).AsString() == StringView(u8"12, 16"));

    // A distance field bakes one size: the ramp's row goes while it does.
    gui::PropertyWrite mode;
    mode.Value = gui::Variant(static_cast<i64>(pipeline::FontBakeMode::DistanceField));
    page->Edit(u8"mode", mode);
    CHECK_FALSE(page->Ramp().FindInfo(u8"sizes")->Visible);
    page->Commands().Undo();
    CHECK(page->Ramp().FindInfo(u8"sizes")->Visible);
}

TEST_CASE("font page: Browse on the source row picks another font file of the project's; the relink is one undo step")
{
    Bed bed;
    const String other = PathJoin(bed.project->SourcesRoot().AsView(), u8"faces/bold.otf");
    (void)CreateDirectories(PathParent(other.AsView()));
    const char8_t notAFont[] = u8"x";
    REQUIRE(WriteFile(other.AsView(), Span<const byte>(reinterpret_cast<const byte*>(notAFont), 1u)).IsOk());
    REQUIRE(WriteFile(PathJoin(bed.project->SourcesRoot().AsView(), u8"readme.txt").AsView(), Span<const byte>(reinterpret_cast<const byte*>(notAFont), 1u)).IsOk());
    UniquePtr<FontEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    page->BrowseSource();
    REQUIRE(bed.picked.OnPicked);
    CHECK(bed.picked.Current == String(u8"roboto.ttf"));
    REQUIRE(bed.picked.Items.Size() == 2u); // the font files, not the text
    CHECK(bed.picked.Items[0].Key == String(u8"faces/bold.otf"));
    CHECK(bed.picked.Items[1].Key == String(u8"roboto.ttf"));
    bed.picked.OnPicked(u8"faces/bold.otf");
    CHECK(page->SourceFile() == StringView(u8"faces/bold.otf"));
    REQUIRE(page->Commands().CanUndo());
    page->Commands().Undo();
    CHECK(page->SourceFile() == StringView(u8"roboto.ttf"));
}
