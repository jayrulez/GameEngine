// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The vegetation mask page over its markup (Data/Editor/ImagePage.xml) and a scratch project. A
// painted mask's planes show one at a time with the density under the pointer; its size is
// read-only once painted, while its plane count can be edited (one undo step, keeping the planes
// as the cook would). A blank mask says so and its size is editable. An imported mask's planes are
// its image's channels, and every option is read-only.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.image;
import foundation.image.io;
import foundation.vfs;
import foundation.vegetation.resource;
import pipeline.core;
import vegetation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.vegetation;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;
namespace image = foundation::image;

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

    // A project with a painted mask ("Grass": 4 x 2, two planes, the second a ramp), a blank mask
    // ("Fresh") and one imported from a 2 x 1 image ("Field").
    struct Bed
    {
        String dir = String(u8"scratch_vegetation_mask_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        foundation::content::Instance* grass = nullptr;
        foundation::content::Instance* fresh = nullptr;
        foundation::content::Instance* field = nullptr;

        Bed()
        {
            pipeline::RegisterVegetationMaskAsset();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Group* top = project->SourceDb().RootGroup();
            grass = top->CreateInstance(u8"Grass", pipeline::VegetationMaskAsset::StaticType());
            fresh = top->CreateInstance(u8"Fresh", pipeline::VegetationMaskAsset::StaticType());
            field = top->CreateInstance(u8"Field", pipeline::VegetationMaskAsset::StaticType());
            REQUIRE(grass != nullptr);
            REQUIRE(fresh != nullptr);
            REQUIRE(field != nullptr);

            pipeline::VegetationMaskAsset mask;
            mask.width = 4;
            mask.height = 2;
            mask.planeCount = 2;
            REQUIRE(grass->WriteObject(mask).IsOk());
            REQUIRE(fresh->WriteObject(mask).IsOk());
            byte densities[16] = {};
            for (usize i = 0; i < 8; ++i)
                densities[8 + i] = static_cast<byte>(i * 30); // plane 2: a ramp; plane 1: nothing
            densities[0] = static_cast<byte>(255);
            REQUIRE(grass->WriteData(foundation::vegetation::kVegetationMaskStream, Span<const byte>(densities, 16)).IsOk());

            image::Image picture(2, 1, image::PixelFormat::RGBA8);
            Span<u8> rgba = picture.PixelDataMut();
            const u8 texels[8] = {10, 20, 30, 40, 50, 60, 70, 80};
            for (usize i = 0; i < 8; ++i)
                rgba[i] = texels[i];
            (void)CreateDirectory(project->SourcesRoot().AsView()); // may already exist
            REQUIRE(image::io::SaveImage(picture, PathJoin(project->SourcesRoot().AsView(), u8"field.png").AsView(), image::io::ImageFileFormat::PNG).IsOk());
            pipeline::VegetationMaskAsset imported;
            imported.fileName = foundation::vfs::SourcePath(u8"field.png");
            REQUIRE(field->WriteObject(imported).IsOk());
        }
        ~Bed()
        {
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<VegetationMaskPage> Open(foundation::content::Instance& instance)
        {
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<VegetationMaskPage>(DefaultAllocator(), context, Move(services), instance);
        }
    };

    bool ReadOnly(VegetationMaskPage& page, StringView path)
    {
        Array<gui::PropertyInfo> rows;
        page.Properties().GetProperties(StringView{}, rows);
        for (const gui::PropertyInfo& info : rows)
            if (info.Name.AsView() == path)
                return info.ReadOnly;
        FAIL("no such row");
        return false;
    }
}

TEST_CASE("vegetation mask page: a painted mask's planes one at a time, the density under the pointer, its size read only")
{
    Bed bed;
    UniquePtr<VegetationMaskPage> page = bed.Open(*bed.grass);
    REQUIRE_FALSE(page->LoadFailed());
    CHECK(page->Painted());
    CHECK_FALSE(page->Imported());
    CHECK(page->PlaneCount() == 2u);
    REQUIRE(page->PlaneChoice() != nullptr);
    CHECK(page->PlaneChoice()->GetSelectedIndex() == 0);
    u8 density = 0;
    REQUIRE(page->DensityAt(0, 0, density));
    CHECK(density == 255u);
    CHECK(page->Fact(u8"Covered") == String(u8"13%")); // one texel of eight

    page->ShowPlane(1);
    CHECK(page->ShownPlane() == 1u);
    CHECK(page->PlaneChoice()->GetSelectedIndex() == 1);
    REQUIRE(page->DensityAt(3, 1, density));
    CHECK(density == 210u); // the last value of the ramp
    CHECK_FALSE(page->DensityAt(4, 0, density)); // outside the mask
    CHECK(page->Preview()->View().HasImage());

    CHECK(ReadOnly(*page, u8"width"));
    CHECK(ReadOnly(*page, u8"height"));
    CHECK_FALSE(ReadOnly(*page, u8"planeCount"));
    CHECK_FALSE(page->Notice(u8"size").IsEmpty());
    CHECK(page->Fact(u8"Painted") == String(u8"Yes"));
}

TEST_CASE("vegetation mask page: a plane added keeps those painted, one undo step")
{
    Bed bed;
    UniquePtr<VegetationMaskPage> page = bed.Open(*bed.grass);
    page->ShowPlane(1);
    gui::PropertyWrite write;
    write.Value = gui::Variant(static_cast<i64>(3));
    page->Edit(u8"planeCount", write);
    CHECK(page->PlaneCount() == 3u);
    CHECK(page->ShownPlane() == 1u); // the shown plane stays
    u8 density = 0;
    REQUIRE(page->DensityAt(3, 1, density));
    CHECK(density == 210u);
    page->ShowPlane(2);
    REQUIRE(page->DensityAt(0, 0, density));
    CHECK(density == 0u); // a new plane starts empty

    page->Commands().Undo();
    CHECK(page->PlaneCount() == 2u);
    CHECK(page->ShownPlane() == 1u); // plane 2 is gone, so the last remaining plane shows
}

TEST_CASE("vegetation mask page: a blank mask says nothing is painted and keeps its size editable")
{
    Bed bed;
    UniquePtr<VegetationMaskPage> page = bed.Open(*bed.fresh);
    CHECK_FALSE(page->Painted());
    CHECK_FALSE(page->Preview()->View().HasImage());
    CHECK_FALSE(ReadOnly(*page, u8"width"));
    CHECK(page->Notice(u8"size").IsEmpty());
    CHECK(page->Fact(u8"Painted") == String(u8"Not yet"));
}

TEST_CASE("vegetation mask page: an imported mask's planes are its image's channels, its options read only")
{
    Bed bed;
    UniquePtr<VegetationMaskPage> page = bed.Open(*bed.field);
    CHECK(page->Imported());
    CHECK(page->PlaneCount() == 4u);
    page->ShowPlane(2);
    u8 density = 0;
    REQUIRE(page->DensityAt(1, 0, density));
    CHECK(density == 70u); // the second texel's blue
    CHECK(ReadOnly(*page, u8"width"));
    CHECK(ReadOnly(*page, u8"planeCount"));
    CHECK(page->Fact(u8"Source Size") == String(u8"2 x 1"));
}
