// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// HeightfieldEditorPage tests: the factory's type, then the page from the image pages' markup over
// a heightfield in a project. Its heights show from the source's lowest to its highest, a sample
// reads in metres by the asset's Y range, the source's facts are read only and follow an edit of
// the range (one undo step), and a blank heightfield is reported in the picture's place.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import heightfield.pipeline;
import foundation.vfs;
import foundation.gui;
import foundation.gui.toolkit;
import editor.heightfield;
import editor.core;
import editor.app;

using namespace foundation::core;
using namespace editor;
namespace image = foundation::image;
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

    // A project with a 3x1 heightmap ("ridge.png": 64, 192, 128 of 255) under a heightfield from
    // -10 m to 90 m ("Ridge"), and a blank heightfield ("Flat").
    struct Bed
    {
        String dir = String(u8"scratch_heightfield_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* ridge = nullptr;
        foundation::content::Instance* flat = nullptr;

        Bed()
        {
            pipeline::RegisterHeightfieldAsset(); // read back by its type, as the editor registers it
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            const u8 heights[] = {64, 192, 128};
            image::Image map(3, 1, image::PixelFormat::R8, Span<const u8>(heights, 3));
            REQUIRE(image::io::SaveImage(map, PathJoin(project->SourcesRoot().AsView(), u8"ridge.png").AsView(), image::io::ImageFileFormat::PNG).IsOk());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            ridge = top->CreateInstance(u8"Ridge", pipeline::HeightfieldAsset::StaticType());
            flat = top->CreateInstance(u8"Flat", pipeline::HeightfieldAsset::StaticType());
            REQUIRE(ridge != nullptr);
            REQUIRE(flat != nullptr);
            pipeline::HeightfieldAsset asset;
            asset.minY = -10.0f;
            asset.maxY = 90.0f;
            REQUIRE(flat->WriteObject(asset).IsOk());
            asset.fileName = foundation::vfs::SourcePath(u8"ridge.png");
            REQUIRE(ridge->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<HeightfieldEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<HeightfieldEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };
}

TEST_CASE("HeightfieldEditor registers a factory the registry routes for HeightfieldAsset")
{
    const app::AssetPageServicesSource none; // no services, only the registration; declared first so it outlives the context
    EditorContext context{DefaultAllocator()};
    RegisterHeightfieldEditor(context, none);
    IEditorPageFactory* found = context.Pages().FindFactory(pipeline::HeightfieldAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::HeightfieldAsset::StaticType());
}

TEST_CASE("heightfield page: its heights from the lowest to the highest, a sample in metres, its range's facts following an edit")
{
    Bed bed;
    UniquePtr<HeightfieldEditorPage> page = bed.Open(*bed.ridge);
    REQUIRE_FALSE(page->LoadFailed());
    app::ImageInspectView& view = page->Preview()->View();
    REQUIRE(view.HasImage());
    app::ImagePixel pixel;
    REQUIRE(view.ValueAt(0, 0, pixel));
    CHECK(pixel.r == 0); // the lowest, black
    REQUIRE(view.ValueAt(1, 0, pixel));
    CHECK(pixel.r == 255); // the highest, white
    REQUIRE(view.ValueAt(2, 0, pixel));
    CHECK(pixel.r == 128);

    u16 sample = 0;
    REQUIRE(page->SampleAt(1, 0, sample));
    CHECK(sample == 192u * 257u);
    CHECK(page->HeightOf(65535) == doctest::Approx(90.0f));
    CHECK(page->HeightOf(0) == doctest::Approx(-10.0f));
    CHECK_FALSE(page->SampleAt(3, 0, sample));
    CHECK(page->Fact(u8"Source Size") == StringView(u8"3 x 1"));
    CHECK(page->Fact(u8"Highest") == StringView(u8"65.29 m")); // 192/255 of the way from -10 to 90

    gui::PropertyWrite write;
    write.Value = gui::Variant(190.0);
    page->Edit(u8"maxY", write);
    CHECK(page->Fact(u8"Highest") == StringView(u8"140.59 m"));
    page->Commands().Undo();
    CHECK(page->Fact(u8"Highest") == StringView(u8"65.29 m"));
}

TEST_CASE("heightfield page: a blank heightfield said in the picture's place")
{
    Bed bed;
    UniquePtr<HeightfieldEditorPage> page = bed.Open(*bed.flat);
    CHECK_FALSE(page->LoadFailed());
    CHECK_FALSE(page->Preview()->View().HasImage());
    CHECK(app::FindById<gui::Node>(*page->Content(), u8"image-empty")->IsDisplayed());
    CHECK(page->SourceFile() == StringView(u8"Not set"));
}
