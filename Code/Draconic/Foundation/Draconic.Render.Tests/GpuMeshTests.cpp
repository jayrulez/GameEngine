// Slice 2 (mesh upload) - the mesh GPU cache uploads a StaticMesh's vertex/index
// streams to RHI buffers on first use and reuses them after. Exercised on the Null RHI.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.rhi;
import draconic.rhi.null;
import draconic.geometry;
import draconic.render;

using namespace draconic::foundation;
using namespace draconic::render;
namespace rhi = draconic::rhi;
namespace geometry = draconic::geometry;

TEST_CASE("mesh GPU cache: uploads on first use, reuses after, frees on clear")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuMeshCache cache(device);

    RefPtr<geometry::StaticMesh> cube = geometry::Primitives::Cube(1.0f);

    const GpuMesh* g = cache.GetOrUpload(cube.Get());
    REQUIRE(g != nullptr);
    CHECK(g->vertexBuffer != nullptr);
    CHECK(g->indexBuffer != nullptr);
    CHECK(g->indexCount == cube->IndexCount()); // 36
    CHECK(g->indexFormat == rhi::IndexFormat::UInt32);
    CHECK(cache.Size() == 1);

    // second request for the same mesh returns the same cached entry (no re-upload)
    const GpuMesh* again = cache.GetOrUpload(cube.Get());
    CHECK(again == g);
    CHECK(cache.Size() == 1);

    // a different mesh is a distinct entry
    RefPtr<geometry::StaticMesh> sphere = geometry::Primitives::Sphere(1.0f, 8, 4);
    const GpuMesh* s = cache.GetOrUpload(sphere.Get());
    REQUIRE(s != nullptr);
    CHECK(s->indexCount == sphere->IndexCount());
    CHECK(cache.Size() == 2);

    cache.Clear();
    CHECK(cache.Size() == 0);
}

TEST_CASE("mesh GPU cache: null + empty meshes upload nothing")
{
    rhi::null::NullDevice device{DefaultAllocator()};
    GpuMeshCache cache(device);
    CHECK(cache.GetOrUpload(nullptr) == nullptr);
    geometry::StaticMesh empty;
    CHECK(cache.GetOrUpload(&empty) == nullptr); // no vertices/indices
    CHECK(cache.Size() == 0);
}
