// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MeshEditorPage tests: the viewer's stat-line readout, a free, pure function; then the page from
// the preview pages' markup over a mesh in a project, without a host (so no viewport, no cooked
// product): the preview's place says the mesh is not cooked, the preview's own choices sit on its
// bar, and the preview's material is kept for the mesh with the project.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.settings;
import foundation.geometry;
import geometry.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.geometry;

using namespace foundation::core;
namespace geometry = foundation::geometry;

TEST_CASE("MeshStatLines reports counts, bounds, skinning, and per-submesh rows")
{
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 2.0f);
    REQUIRE(cube.Get() != nullptr);

    Array<String> lines = editor::MeshStatLines(*cube);

    // Header block: name, vertices, indices, submeshes, bounds, skinned - then one line per submesh.
    REQUIRE(lines.Size() >= 6 + cube->subMeshes.Size());

    auto hasPrefix = [&](StringView prefix) -> bool
    {
        for (const String& line : lines)
        {
            if (line.AsView().StartsWith(prefix))
            {
                return true;
            }
        }
        return false;
    };

    CHECK(hasPrefix(u8"Name:"));
    CHECK(hasPrefix(u8"Vertices:"));
    CHECK(hasPrefix(u8"Indices:"));
    CHECK(hasPrefix(u8"Submeshes:"));
    CHECK(hasPrefix(u8"Bounds:"));
    // A unit cube of size 2 is not skinned.
    CHECK(hasPrefix(u8"Skinned: no"));

    // One indented submesh row per submesh, each naming its material index.
    usize submeshRows = 0;
    for (const String& line : lines)
    {
        if (line.AsView().StartsWith(u8"  ["))
        {
            ++submeshRows;
        }
    }
    CHECK(submeshRows == cube->subMeshes.Size());
    CHECK(submeshRows >= 1);
}

TEST_CASE("MeshStatLines counts match the mesh geometry")
{
    RefPtr<geometry::StaticMesh> sphere = geometry::Primitives::Sphere(DefaultAllocator(), 0.5f, 16, 12);
    REQUIRE(sphere.Get() != nullptr);
    REQUIRE(sphere->VertexCount() > 0);
    REQUIRE(sphere->IndexCount() > 0);

    Array<String> lines = editor::MeshStatLines(*sphere);

    const String vExpected = Format(u8"Vertices: {}", sphere->VertexCount());
    const String iExpected = Format(u8"Indices: {}", sphere->IndexCount());

    bool sawV = false;
    bool sawI = false;
    for (const String& line : lines)
    {
        if (line.AsView() == vExpected.AsView())
        {
            sawV = true;
        }
        if (line.AsView() == iExpected.AsView())
        {
            sawI = true;
        }
    }
    CHECK(sawV);
    CHECK(sawI);
}

TEST_CASE("MeshStatLines reports the LOD chain (levels, per-level triangles, thresholds)")
{
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    REQUIRE(cube.Get() != nullptr);
    // Fake a 2-level chain: level 1 reuses the first 12 indices as its range.
    cube->lodCount = 2;
    cube->lodSubMeshes.PushBack(geometry::SubMesh{0, 12, 0, geometry::PrimitiveType::Triangles});
    cube->lodCoverage.PushBack(1.0f);
    cube->lodCoverage.PushBack(0.25f);

    Array<String> lines = editor::MeshStatLines(*cube);
    bool sawLevels = false, sawLod0 = false, sawLod1 = false;
    for (const String& line : lines)
    {
        if (line.AsView() == StringView(u8"LOD levels: 2"))
        {
            sawLevels = true;
        }
        if (line.AsView() == StringView(u8"  LOD 0: 12 triangles"))
        {
            sawLod0 = true; // cube: 36 indices = 12 triangles
        }
        if (line.AsView() == StringView(u8"  LOD 1: 4 triangles  |  below 0.250 coverage"))
        {
            sawLod1 = true; // 12 indices = 4 triangles
        }
    }
    CHECK(sawLevels);
    CHECK(sawLod0);
    CHECK(sawLod1);

    // Chainless meshes show no LOD block.
    RefPtr<geometry::StaticMesh> plain = geometry::Primitives::Cube(DefaultAllocator(), 1.0f);
    for (const String& line : editor::MeshStatLines(*plain))
    {
        CHECK(line.AsView() != StringView(u8"LOD levels: 1"));
    }
}

namespace
{
    namespace gui = foundation::gui;

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

    // A project with a static mesh asset ("Crate"), nothing cooked, and its editor settings.
    struct Bed
    {
        String dir = String(u8"scratch_mesh_page_project");
        UniquePtr<editor::EditorProject> project;
        foundation::settings::Settings store{DefaultAllocator()};
        editor::app::AssetPageServicesSource none; // the registration's: no services, outliving the context below
        editor::EditorContext context{DefaultAllocator()};
        foundation::content::Instance* crate = nullptr;

        Bed()
        {
            pipeline::RegisterMeshAssets(); // read back by its type, as the editor registers it
            editor::RegisterMeshEditor(context, none); // and the prefs' section
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            context.SetProjectEditorSettings(&store);
            crate = project->SourceDb().RootGroup()->CreateInstance(u8"Crate", pipeline::StaticMeshAsset::StaticType());
            REQUIRE(crate != nullptr);
            pipeline::StaticMeshAsset asset;
            REQUIRE(crate->WriteObject(asset).IsOk());
        }
        ~Bed()
        {
            context.SetProjectEditorSettings(nullptr);
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        UniquePtr<editor::MeshEditorPage> Open()
        {
            editor::app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            return MakeUnique<editor::MeshEditorPage>(DefaultAllocator(), context, Move(services), *crate, pipeline::StaticMeshAsset::StaticType());
        }
    };
}

TEST_CASE("mesh page: not cooked, said in the preview's place; its choices on the preview's bar; its material kept with the project")
{
    Bed bed;
    REQUIRE(bed.context.Pages().FindFactory(pipeline::StaticMeshAsset::StaticType()) != nullptr);
    REQUIRE(bed.context.Pages().FindFactory(pipeline::SkinnedMeshAsset::StaticType()) != nullptr);

    UniquePtr<editor::MeshEditorPage> page = bed.Open();
    REQUIRE_FALSE(page->LoadFailed());
    REQUIRE(page->Pane() != nullptr);
    CHECK(page->Pane()->Viewport() == nullptr); // no host here
    CHECK_FALSE(page->Pane()->Showing());
    CHECK(editor::app::FindById<gui::Node>(*page->Content(), u8"preview-empty")->IsDisplayed());
    CHECK(editor::app::FindById<gui::Label>(*page->Content(), u8"preview-empty-title")->GetText() == StringView(u8"Not cooked yet"));
    CHECK(page->Fact(u8"Status") == StringView(u8"Not cooked yet"));

    // The material slot and the LOD choice follow the bar's own separator, the LOD hidden for a
    // mesh with no chain.
    REQUIRE(page->MaterialSlot() != nullptr);
    gui::Node* own = editor::app::FindById<gui::Node>(*page->Content(), u8"preview-own");
    REQUIRE(own != nullptr);
    gui::Node* bar = own->GetParent();
    CHECK(bar->GetChildIndex(page->MaterialSlot()) == bar->GetChildIndex(own) + 1);
    CHECK(bar->GetChildIndex(page->LodChoice()) == bar->GetChildIndex(own) + 2);
    CHECK_FALSE(page->LodChoice()->IsDisplayed());

    const Guid wood{0x12, 0x34};
    page->SetPreviewMaterial(wood);
    CHECK(page->PreviewMaterial() == wood);
    page.Reset();
    UniquePtr<editor::MeshEditorPage> again = bed.Open();
    CHECK(again->PreviewMaterial() == wood); // kept for this mesh
}
