// MeshEditorPage tests (headless): the viewer's stat-line readout is a free, pure function so
// it is covered here without a live host/renderer. The page's GPU orbit preview + product
// binding + viewport lifecycle need a live application host (like MaterialPage/SceneEditorPage)
// and are exercised in the editor app, not here.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.geometry;
import draconic.editor.scene;

using namespace draconic::foundation;
namespace geometry = draconic::geometry;

TEST_CASE("MeshStatLines reports counts, bounds, skinning, and per-submesh rows")
{
    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(2.0f);
    REQUIRE(cube.Get() != nullptr);

    Array<String> lines = draconic::editor::MeshStatLines(*cube);

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
    RefPtr<geometry::StaticMesh> sphere = geometry::Primitives::Sphere(0.5f, 16, 12);
    REQUIRE(sphere.Get() != nullptr);
    REQUIRE(sphere->VertexCount() > 0);
    REQUIRE(sphere->IndexCount() > 0);

    Array<String> lines = draconic::editor::MeshStatLines(*sphere);

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
