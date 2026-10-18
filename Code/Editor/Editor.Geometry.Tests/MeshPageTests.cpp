// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// MeshEditorPage tests (headless): the viewer's stat-line readout is a free, pure function so
// it is covered here without a live host/renderer. The page's GPU orbit preview + product
// binding + viewport lifecycle need a live application host (like MaterialPage/SceneEditorPage)
// and are exercised in the editor app, not here.

#include <doctest/doctest.h>

#include "Core/Prelude.h"

import foundation.core;
import foundation.geometry;
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
