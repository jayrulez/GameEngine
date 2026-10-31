// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Meshes as resources: cook a mesh (captured into a source, written to the content DB), then
// build it back through the ResourceManager via the factory and verify the runtime mesh
// round-trips its vertices, indices, submeshes and bounds. Covers the static and skinned paths.
#include <doctest/doctest.h>
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;
import foundation.vfs;
import foundation.content;
import foundation.resource;
import foundation.geometry;
import foundation.geometry.resource;

using namespace foundation::core;
using namespace foundation::vfs;
using namespace foundation::resource;
using namespace foundation::geometry;

namespace
{
    void RemoveTree()
    {
        FileDelete(u8"scratch_mesh_res_db/cube.rasset");
        FileDelete(u8"scratch_mesh_res_db/skinned.rasset");
        RemoveDirectory(u8"scratch_mesh_res_db");
    }
}

TEST_CASE("static mesh resource: cube round-trips through the resource manager")
{
    GlobalTypeRegistry().Register(StaticMeshSource::StaticType());
    RegisterSerializable<StaticMeshSource>();
    GlobalTypeRegistry().Register(StaticMesh::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_mesh_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"cube", StaticMeshSource::StaticType());
        id = inst->Id();

        RefPtr<StaticMesh> cube = Primitives::Cube(DefaultAllocator(), 2.0f);
        StaticMeshSource src;
        StaticMeshSource::FromMesh(*cube, src);
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    StaticMeshFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<StaticMesh> cube = manager.Bind<StaticMesh>(id);
    REQUIRE(cube);
    CHECK(cube->VertexCount() == 24);
    CHECK(cube->IndexCount() == 36);
    CHECK(cube->subMeshes.Size() == 1);
    CHECK_FALSE(cube->IsSkinned());
    CHECK(cube->bounds.min.x == doctest::Approx(-1.0f));
    CHECK(cube->bounds.max.z == doctest::Approx(1.0f));

    RemoveTree();
}

TEST_CASE("static mesh resource: async load matches the sync product")
{
    GlobalTypeRegistry().Register(StaticMeshSource::StaticType());
    RegisterSerializable<StaticMeshSource>();
    GlobalTypeRegistry().Register(StaticMesh::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_mesh_res_db", DefaultAllocator());
    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"cube", StaticMeshSource::StaticType());
        id = inst->Id();
        RefPtr<StaticMesh> cube = Primitives::Cube(DefaultAllocator(), 2.0f);
        StaticMeshSource src;
        StaticMeshSource::FromMesh(*cube, src);
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    StaticMeshFactory factory(DefaultAllocator());

    ResourceManager syncManager(DefaultAllocator(), db);
    syncManager.AddFactory(&factory);
    Proxy<StaticMesh> a = syncManager.Bind<StaticMesh>(id);
    REQUIRE(a);

    JobSystem jobs(DefaultAllocator());
    ResourceManager asyncManager(DefaultAllocator(), db, &jobs);
    asyncManager.AddFactory(&factory);
    Proxy<StaticMesh> b = asyncManager.BindAsync<StaticMesh>(id);
    asyncManager.WaitAll();
    REQUIRE(b);
    CHECK(b.Handle()->State() == ResourceState::Ready);

    CHECK(b->VertexCount() == a->VertexCount());
    CHECK(b->IndexCount() == a->IndexCount());
    CHECK(b->subMeshes.Size() == a->subMeshes.Size());
    CHECK(b->bounds.min.x == doctest::Approx(a->bounds.min.x));
    CHECK(b->bounds.max.z == doctest::Approx(a->bounds.max.z));

    RemoveTree();
}

TEST_CASE("static mesh resource: many concurrent async decodes run without a race")
{
    // N meshes bound async at once: N DecodeStages reading the content DB and registries and doing
    // the heavy vertex and index blob copy concurrently on workers. Run under TSAN.
    GlobalTypeRegistry().Register(StaticMeshSource::StaticType());
    RegisterSerializable<StaticMeshSource>();
    GlobalTypeRegistry().Register(StaticMesh::StaticType());

    RemoveDirectory(u8"scratch_mesh_conc_db");
    NativeFileSystem mount(u8"scratch_mesh_conc_db", DefaultAllocator());

    constexpr int kCount = 10;
    Array<Guid> ids;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        RefPtr<StaticMesh> cube = Primitives::Cube(DefaultAllocator(), 2.0f);
        StaticMeshSource src;
        StaticMeshSource::FromMesh(*cube, src);
        for (int i = 0; i < kCount; ++i)
        {
            char8_t name[8] = {u8'm', u8'e', u8's', u8'h', static_cast<char8_t>(u8'0' + i / 10),
                               static_cast<char8_t>(u8'0' + i % 10), 0};
            auto* inst =
                db.RootGroup()->CreateInstance(StringView(name), StaticMeshSource::StaticType());
            REQUIRE(inst->WriteObject(src).IsOk());
            ids.PushBack(inst->Id());
        }
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    StaticMeshFactory factory(DefaultAllocator());
    JobSystem jobs(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db, &jobs);
    manager.AddFactory(&factory);

    Array<Proxy<StaticMesh>> meshes;
    for (const Guid& id : ids)
    {
        meshes.PushBack(manager.BindAsync<StaticMesh>(id));
    }
    manager.WaitAll();

    for (Proxy<StaticMesh>& mesh : meshes)
    {
        REQUIRE(mesh);
        CHECK(mesh.Handle()->State() == ResourceState::Ready);
        CHECK(mesh->VertexCount() == 24u);
        CHECK(mesh->IndexCount() == 36u);
    }

    RemoveDirectory(u8"scratch_mesh_conc_db");
}

TEST_CASE("skinned mesh resource: round-trips the static + skinning streams")
{
    GlobalTypeRegistry().Register(StaticMeshSource::StaticType());
    GlobalTypeRegistry().Register(SkinnedMeshSource::StaticType());
    RegisterSerializable<SkinnedMeshSource>();
    GlobalTypeRegistry().Register(StaticMesh::StaticType());
    GlobalTypeRegistry().Register(SkinnedMesh::StaticType());

    RemoveTree();
    NativeFileSystem mount(u8"scratch_mesh_res_db", DefaultAllocator());

    Guid id;
    {
        foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                              u8".rasset");
        auto* inst = db.RootGroup()->CreateInstance(u8"skinned", SkinnedMeshSource::StaticType());
        id = inst->Id();

        RefPtr<SkinnedMesh> mesh = MakeRef<SkinnedMesh>(DefaultAllocator());
        mesh->skeletonIndex = 7;
        for (u32 i = 0; i < 3; ++i)
        {
            mesh->vertices.PushBack(StaticMeshVertex{Float3{static_cast<f32>(i), 0, 0},
                                                     Float3{0, 1, 0}, Float2{0, 0}, 0xFFFFFFFFu,
                                                     Float3{1, 0, 0}});
            VertexSkinning s{};
            s.joints[0] = static_cast<u16>(i);
            s.weights = Float4{1, 0, 0, 0};
            mesh->skinning.PushBack(s);
        }
        mesh->indices.Resize(3);
        mesh->indices.AddTriangle(0, 1, 2);

        SkinnedMeshSource src;
        SkinnedMeshSource::FromMesh(*mesh, src);
        REQUIRE(inst->WriteObject(src).IsOk());
    }

    foundation::content::ContentDatabase db(foundation::core::DefaultAllocator(), mount, foundation::core::BinarySerializerFactory(),
                                          u8".rasset");
    SkinnedMeshFactory factory(DefaultAllocator());
    ResourceManager manager(DefaultAllocator(), db);
    manager.AddFactory(&factory);

    Proxy<SkinnedMesh> mesh = manager.Bind<SkinnedMesh>(id);
    REQUIRE(mesh);
    CHECK(mesh->VertexCount() == 3);
    CHECK(mesh->IndexCount() == 3);
    CHECK(mesh->IsSkinned());
    CHECK(mesh->skeletonIndex == 7);
    REQUIRE(mesh->SkinningStream().Size() == 3);
    CHECK(mesh->SkinningStream()[2].joints[0] == 2);

    // the skinned product is also usable as a StaticMesh
    Proxy<StaticMesh> asStatic = manager.Bind<StaticMesh>(id);
    REQUIRE(asStatic);
    CHECK(asStatic->VertexCount() == 3);
    CHECK(asStatic->IsSkinned());

    RemoveTree();
}
