// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// ImageEditorPage tests: the factory's type, then the page from the image pages' markup
// (Data/Editor/ImagePage.gml) over an image in a project: its picture decoded from the source file
// and its pixels read, its facts read only, an edit of its colour space one undo step, a source
// file that is not there said in the picture's place, and an asset that did not read.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import image.pipeline;
import foundation.vfs;
import foundation.gui;
import foundation.gui.toolkit;
import editor.image;
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

    // A project with a 4x2 picture ("dot.png": its top left pixel red, half transparent), an image
    // over it ("Dot"), one over a file that is not there ("Gone") and one with nothing written
    // ("Unread").
    struct Bed
    {
        String dir = String(u8"scratch_image_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* dot = nullptr;
        foundation::content::Instance* gone = nullptr;
        foundation::content::Instance* unread = nullptr;

        Bed()
        {
            pipeline::RegisterImageAsset(); // read back by its type, as the editor registers it
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            image::Image picture(4, 2, image::PixelFormat::RGBA8);
            Span<u8> pixels = picture.PixelDataMut();
            for (usize i = 0; i < pixels.Size(); ++i)
                pixels[i] = 255;
            pixels[1] = 0;
            pixels[2] = 0;
            pixels[3] = 128;
            REQUIRE(image::io::SaveImage(picture, PathJoin(project->SourcesRoot().AsView(), u8"dot.png").AsView(), image::io::ImageFileFormat::PNG).IsOk());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            dot = top->CreateInstance(u8"Dot", pipeline::ImageAsset::StaticType());
            gone = top->CreateInstance(u8"Gone", pipeline::ImageAsset::StaticType());
            unread = top->CreateInstance(u8"Unread", pipeline::ImageAsset::StaticType());
            REQUIRE(dot != nullptr);
            REQUIRE(gone != nullptr);
            REQUIRE(unread != nullptr);
            pipeline::ImageAsset asset;
            asset.fileName = foundation::vfs::SourcePath(u8"dot.png");
            REQUIRE(dot->WriteObject(asset).IsOk());
            asset.fileName = foundation::vfs::SourcePath(u8"gone.png");
            REQUIRE(gone->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<ImageEditorPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<ImageEditorPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };
}

TEST_CASE("ImageEditor registers a factory the registry routes for ImageAsset")
{
    const app::AssetPageServicesSource none; // no services: the registration only, before the context it outlives
    EditorContext context{DefaultAllocator()};
    RegisterImageEditor(context, none);
    IEditorPageFactory* found = context.Pages().FindFactory(pipeline::ImageAsset::StaticType());
    REQUIRE(found != nullptr);
    CHECK(found->PrimaryType() == &pipeline::ImageAsset::StaticType());
}

TEST_CASE("image page: the source decoded into the picture, its pixels read, its facts read only, its kind named")
{
    Bed bed;
    UniquePtr<ImageEditorPage> page = bed.Open(*bed.dot);
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Preview() != nullptr);
    app::ImageInspectView& view = page->Preview()->View();
    REQUIRE(view.HasImage());
    CHECK(view.Pixels().Width() == 4u);
    CHECK(view.Pixels().Height() == 2u);
    app::ImagePixel pixel;
    REQUIRE(view.ValueAt(0, 0, pixel));
    CHECK(pixel.r == 255);
    CHECK(pixel.g == 0);
    CHECK(pixel.a == 128);
    CHECK_FALSE(app::FindById<gui::Node>(*page->Content(), u8"image-empty")->IsDisplayed());
    CHECK(app::FindById<gui::Label>(*page->Content(), u8"page-kind")->GetText() == StringView(u8"Image"));

    CHECK(page->SourceFile() == StringView(u8"dot.png"));
    CHECK(page->Fact(u8"Dimensions") == StringView(u8"4 x 2"));
    CHECK(page->Fact(u8"Pixel Format") == StringView(u8"RGBA8"));
    Array<gui::PropertyInfo> facts;
    page->Facts().GetProperties(StringView{}, facts);
    REQUIRE_FALSE(facts.IsEmpty());
    for (const gui::PropertyInfo& info : facts)
        CHECK(info.ReadOnly);
    // The grid: the notices' section, the asset's, then the facts'.
    CHECK(page->Grid()->GetSource() == &page->Sections());
    CHECK(page->Sections().SectionCount() == 3u);
}

TEST_CASE("image page: an edit of the colour space is one undo step, and Save writes it")
{
    Bed bed;
    UniquePtr<ImageEditorPage> page = bed.Open(*bed.dot);
    auto* asset = Cast<pipeline::ImageAsset>(page->Object());
    REQUIRE(asset != nullptr);
    CHECK(asset->colorSpace == image::ImageColorSpace::Srgb);
    gui::PropertyWrite write;
    write.Value = gui::Variant(static_cast<i64>(image::ImageColorSpace::Linear));
    page->Edit(u8"colorSpace", write);
    CHECK(asset->colorSpace == image::ImageColorSpace::Linear);
    CHECK(page->IsDirty());
    page->Commands().Undo();
    CHECK(asset->colorSpace == image::ImageColorSpace::Srgb);
    CHECK(page->Properties().GetValue(u8"colorSpace", 0).AsInt() == static_cast<i64>(image::ImageColorSpace::Srgb));
    page->Commands().Redo();
    CHECK(asset->colorSpace == image::ImageColorSpace::Linear);
    REQUIRE(page->Save().IsOk());
    CHECK_FALSE(page->IsDirty());
    RefPtr<ISerializable> saved = bed.dot->ReadObject();
    auto* read = Cast<pipeline::ImageAsset>(saved.Get());
    REQUIRE(read != nullptr);
    CHECK(read->colorSpace == image::ImageColorSpace::Linear);
}

TEST_CASE("image page: a source file that is not there is said in the picture's place; an unread asset in the grid's")
{
    Bed bed;
    UniquePtr<ImageEditorPage> gone = bed.Open(*bed.gone);
    CHECK_FALSE(gone->LoadFailed());
    CHECK_FALSE(gone->Preview()->View().HasImage());
    CHECK(app::FindById<gui::Node>(*gone->Content(), u8"image-empty")->IsDisplayed());
    CHECK_FALSE(app::FindById<gui::Node>(*gone->Content(), u8"image-stage")->IsDisplayed());
    CHECK(app::FindById<gui::Label>(*gone->Content(), u8"image-empty-text")->GetText() == StringView(u8"gone.png is missing or did not decode."));

    UniquePtr<ImageEditorPage> unread = bed.Open(*bed.unread);
    CHECK(unread->LoadFailed());
    CHECK(app::FindById<gui::Node>(*unread->Content(), u8"page-state")->IsDisplayed());
    CHECK_FALSE(unread->Grid()->IsDisplayed());
    CHECK_FALSE(unread->Save().IsOk());
}
