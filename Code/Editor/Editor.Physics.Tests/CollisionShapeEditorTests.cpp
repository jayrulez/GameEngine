// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The collision shape editor's labels, its outline mesh and its thumbnail generator, headless.
// Ported back from the Beef port's Editor.Physics.Tests (2026-09-20); the page-factory routing
// case is not here because the factory takes a UI host only a window can make.
#include <doctest/doctest.h>
#include <filesystem>
#include "Core/Prelude.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.scene;
import foundation.geometry;
import foundation.physics.resource;
import physics.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.geometry.resource;
import editor.core;
import editor.app;
import editor.physics;

using namespace foundation::core;
namespace scene = foundation::scene;
namespace geometry = foundation::geometry;
namespace physics = foundation::physics;
namespace content = foundation::content;

TEST_CASE("collision editor: the cook labels name the kind, and the outline becomes a lit triangle mesh")
{
    CHECK(editor::CollisionShapeEditorPage::CookLabel(pipeline::CollisionCookKind::ConvexHull) ==
          StringView(u8"Convex hull (dynamic)"));
    CHECK(editor::CollisionShapeEditorPage::CookLabel(pipeline::CollisionCookKind::TriangleMesh) ==
          StringView(u8"Triangle mesh (static)"));

    auto shapeRef = MakeRef<physics::CollisionShape>(DefaultAllocator());
    physics::CollisionShape& shape = *shapeRef;
    shape.outline.PushBack(Float3{0, 0, 0});
    shape.outline.PushBack(Float3{1, 0, 0});
    shape.outline.PushBack(Float3{0, 0, 1});
    shape.outline.PushBack(Float3{0, 1, 0});
    shape.outline.PushBack(Float3{1, 1, 0});
    shape.outline.PushBack(Float3{0, 1, 1});
    shape.outline.PushBack(Float3{5, 5, 5}); // a stray vertex past the last whole triangle: dropped
    auto meshRef = MakeRef<geometry::StaticMesh>(DefaultAllocator());
    geometry::StaticMesh& mesh = *meshRef;
    REQUIRE(editor::BuildCollisionOutlineMesh(shape, mesh));
    CHECK(mesh.VertexCount() == 6u);
    CHECK(mesh.IndexCount() == 6u);
    CHECK(mesh.subMeshes.Size() == 1u);
    CHECK(mesh.bounds.max.y == 1.0f);

    // Too few vertices for a triangle: nothing, and the mesh is left empty.
    auto flatRef = MakeRef<physics::CollisionShape>(DefaultAllocator());
    flatRef->outline.PushBack(Float3{0, 0, 0});
    flatRef->outline.PushBack(Float3{1, 0, 0});
    CHECK_FALSE(editor::BuildCollisionOutlineMesh(*flatRef, mesh));
    CHECK(mesh.VertexCount() == 0u);
}

TEST_CASE("collision editor: the thumbnail generator claims the asset type and shares the stage")
{
    UniquePtr<editor::ISceneThumbnailGenerator> generator = editor::CreateCollisionThumbnailGenerator();
    REQUIRE(generator);
    const Span<const StringView> names = generator->AssetTypeNames();
    REQUIRE(names.Size() == 1u);
    CHECK(names[0] == StringView(u8"CollisionShapeAsset"));
    CHECK_FALSE(generator->NeedsPrivateScene());
}

TEST_CASE("collision editor: staging without the render managers fails rather than crashes")
{
    // A resource manager over an empty scratch database: the stage never gets far enough to
    // bind through it, which is the point.
    const std::filesystem::path dir = "scratch_editor_physics_stage";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    {
        foundation::vfs::NativeFileSystem mount(u8"scratch_editor_physics_stage", DefaultAllocator());
        content::ContentDatabase db(DefaultAllocator(), mount, BinarySerializerFactory(), u8".rasset");
        foundation::resource::ResourceManager resources(DefaultAllocator(), db);

        UniquePtr<editor::ISceneThumbnailGenerator> generator =
            editor::CreateCollisionThumbnailGenerator();
        scene::Scene stage(DefaultAllocator(), u8"stage"); // no MeshComponentManager, no lights
        editor::ThumbnailFraming framing;
        CHECK(generator->Stage(Guid{}, stage, resources, framing) == editor::ThumbnailStageStep::Failed);
        generator->Unstage(stage); // harmless with nothing staged
    }
    std::filesystem::remove_all(dir, ec);
}

namespace
{
    namespace gui = foundation::gui;

    RefPtr<gui::Node> LoadPageMarkup(StringView relative)
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
}

TEST_CASE("collision page: its source mesh a slot for what a mesh cooks to, each edit one undo step, no mesh a notice, no collider said in the preview's place")
{
    pipeline::RegisterPhysicsAssets();
    foundation::geometry::RegisterGeometryResourceTypes(); // StaticMeshSource named, as the editor has it
    const String dir(u8"scratch_collision_page_project");
    (void)RemoveDirectoryRecursive(dir.AsView());
    REQUIRE(editor::EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
    UniquePtr<editor::EditorProject> project = editor::EditorProject::Open(DefaultAllocator(), dir.AsView());
    REQUIRE(static_cast<bool>(project));
    editor::EditorContext context{DefaultAllocator()};
    context.SetProject(project.Get());
    foundation::content::Instance* shape = project->SourceDb().RootGroup()->CreateInstance(u8"Hull", pipeline::CollisionShapeAsset::StaticType());
    REQUIRE(shape != nullptr);
    pipeline::CollisionShapeAsset asset;
    REQUIRE(shape->WriteObject(asset).IsOk());
    {
        editor::app::AssetPageServices services;
        services.loadMarkup = [](StringView path) { return LoadPageMarkup(path); };
        editor::CollisionShapeEditorPage page(context, Move(services), *shape);
        REQUIRE_FALSE(page.LoadFailed());
        Array<gui::PropertyInfo> rows;
        page.Properties().GetProperties(StringView{}, rows);
        const gui::PropertyInfo* mesh = nullptr;
        for (const gui::PropertyInfo& row : rows)
            if (row.Name == StringView(u8"sourceMesh"))
                mesh = &row;
        REQUIRE(mesh != nullptr);
        CHECK(mesh->Kind == gui::PropertyKind::Reference);
        REQUIRE(mesh->ReferenceType != nullptr);
        CHECK(StringView(reinterpret_cast<const utf8char*>(mesh->ReferenceType->name)) == StringView(u8"StaticMeshSource"));
        CHECK_FALSE(page.Notice(u8"mesh").IsEmpty());
        CHECK(page.Fact(u8"Collider") == StringView(u8"None: no source mesh"));
        CHECK_FALSE(page.Pane()->Showing());

        const Guid crate{0x77, 0x1};
        utf8char text[37];
        crate.ToChars(text);
        gui::PropertyWrite write;
        write.Value = gui::Variant(StringView(text, 36));
        page.Edit(u8"sourceMesh", write);
        auto* edited = Cast<pipeline::CollisionShapeAsset>(page.Object());
        REQUIRE(edited != nullptr);
        CHECK(edited->sourceMesh == crate);
        CHECK(page.Notice(u8"mesh").IsEmpty());
        CHECK(page.IsDirty());
        page.Commands().Undo();
        CHECK(edited->sourceMesh.IsNil());
        CHECK_FALSE(page.Notice(u8"mesh").IsEmpty());
    }
    context.SetProject(nullptr);
    project.Reset();
    (void)RemoveDirectoryRecursive(dir.AsView());
}

TEST_CASE("collision page: a collider cooked before its mesh was cleared is not shown")
{
    auto cooked = MakeRef<physics::CollisionShape>(DefaultAllocator());
    cooked->outline.PushBack(Float3{0, 0, 0});
    cooked->outline.PushBack(Float3{1, 0, 0});
    cooked->outline.PushBack(Float3{0, 1, 0});
    const Guid mesh{0x77, 0x1};
    CHECK(editor::CollisionShapeEditorPage::ShowsCollider(mesh, cooked.Get()));
    CHECK_FALSE(editor::CollisionShapeEditorPage::ShowsCollider(Guid{}, cooked.Get())); // cleared: the last cook's is not its
    CHECK_FALSE(editor::CollisionShapeEditorPage::ShowsCollider(mesh, nullptr));    // named, not cooked yet
}
