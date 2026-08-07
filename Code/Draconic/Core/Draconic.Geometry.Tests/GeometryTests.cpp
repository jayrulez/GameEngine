// The engine runtime mesh format: vertex/stream sizes, the index buffer, StaticMesh
// geometry ops, and the key design point -- SkinnedMesh IS-A StaticMesh, so its static
// stream is usable anywhere a StaticMesh is, with the skinning stream discoverable via
// the virtual hooks. Plus the procedural primitives.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.geometry;

using namespace draconic::foundation;
using namespace draconic::geometry;

TEST_CASE("stream layouts are the GPU-canonical sizes")
{
    CHECK(sizeof(StaticMeshVertex) == 52); // Float4 tangent (w = TBN handedness)
    CHECK(sizeof(VertexSkinning) == 24);
    CHECK(StaticMesh::VertexStride() == 52); // Float4 tangent (w = TBN handedness)
    CHECK(SkinnedMesh::SkinningStride() == 24);
}

TEST_CASE("index buffer: format, set/get, raw size")
{
    IndexBuffer ib(IndexBuffer::Format::U16);
    CHECK(ib.IndexSize() == 2);
    ib.Resize(3);
    ib.AddTriangle(0, 1, 2);
    CHECK(ib.Count() == 3);
    CHECK(ib.Get(0) == 0);
    CHECK(ib.Get(2) == 2);
    CHECK(ib.DataSize() == 6);
    CHECK(ib.RawData() != nullptr);

    IndexBuffer ib32(IndexBuffer::Format::U32);
    CHECK(ib32.IndexSize() == 4);
    ib32.Resize(2);
    ib32.Set(0, 70000); // exceeds u16 range -> needs 32-bit
    CHECK(ib32.Get(0) == 70000);
}

TEST_CASE("static mesh: generated normals + tangents + bounds on a quad")
{
    RefPtr<StaticMesh> mesh = Primitives::Quad(2.0f, 2.0f);
    REQUIRE(mesh);
    CHECK(mesh->VertexCount() == 4);
    CHECK(mesh->IndexCount() == 6);
    CHECK(mesh->subMeshes.Size() == 1);

    mesh->GenerateNormals();
    for (const StaticMeshVertex& v : mesh->vertices)
    {
        CHECK(v.normal.z == doctest::Approx(1.0f)); // quad faces +Z
        const Float3 t3{v.tangent.x, v.tangent.y, v.tangent.z};
        CHECK(LengthSquared(t3) == doctest::Approx(1.0f)); // unit tangent xyz
        CHECK(Abs(v.tangent.w) == doctest::Approx(1.0f));  // handedness is +-1
    }

    mesh->CalculateBounds();
    CHECK(mesh->bounds.min.x == doctest::Approx(-1.0f));
    CHECK(mesh->bounds.max.y == doctest::Approx(1.0f));
    CHECK(mesh->VertexDataSize() == 4 * 52);
    CHECK(mesh->VertexData() != nullptr);
}

TEST_CASE("skinned mesh IS-A static mesh: static stream is substitutable")
{
    RefPtr<SkinnedMesh> skinned = MakeRef<SkinnedMesh>(DefaultAllocator());
    skinned->skeletonIndex = 3;
    // static stream (inherited)
    skinned->vertices.PushBack(StaticMeshVertex{Float3{0, 0, 0}, Float3{0, 1, 0}, Float2{0, 0},
                                                0xFFFFFFFFu, Float3{1, 0, 0}});
    skinned->vertices.PushBack(StaticMeshVertex{Float3{1, 0, 0}, Float3{0, 1, 0}, Float2{1, 0},
                                                0xFFFFFFFFu, Float3{1, 0, 0}});
    skinned->vertices.PushBack(StaticMeshVertex{Float3{0, 1, 0}, Float3{0, 1, 0}, Float2{0, 1},
                                                0xFFFFFFFFu, Float3{1, 0, 0}});
    // parallel skinning stream
    VertexSkinning s{};
    s.joints[0] = 2;
    s.weights = Float4{1, 0, 0, 0};
    for (u32 i = 0; i < 3; ++i)
    {
        skinned->skinning.PushBack(s);
    }

    // pass it where a StaticMesh& is expected -- the static ops just work
    StaticMesh& asStatic = *skinned;
    asStatic.CalculateBounds();
    CHECK(asStatic.VertexCount() == 3);
    CHECK(asStatic.bounds.max.x == doctest::Approx(1.0f));

    // a consumer holding the base ref can discover + reach the skinning stream
    CHECK(asStatic.IsSkinned());
    CHECK(asStatic.SkinningStream().Size() == 3);
    CHECK(asStatic.SkinningStream()[0].joints[0] == 2);

    // and downcast safely (reflection-based Cast, no RTTI)
    SkinnedMesh* down = Cast<SkinnedMesh>(&asStatic);
    REQUIRE(down != nullptr);
    CHECK(down->skeletonIndex == 3);
    CHECK(down->SkinningDataSize() == 3 * 24);

    // a plain static mesh reports not-skinned + an empty stream
    StaticMesh plain;
    CHECK_FALSE(plain.IsSkinned());
    CHECK(plain.SkinningStream().Size() == 0);
    CHECK(Cast<SkinnedMesh>(&plain) == nullptr);
}

TEST_CASE("primitives: cube + sphere + plane are well-formed")
{
    RefPtr<StaticMesh> cube = Primitives::Cube(2.0f);
    REQUIRE(cube);
    CHECK(cube->VertexCount() == 24); // 4 verts x 6 faces (hard normals)
    CHECK(cube->IndexCount() == 36);
    CHECK(cube->bounds.min.x == doctest::Approx(-1.0f));
    CHECK(cube->bounds.max.z == doctest::Approx(1.0f));

    RefPtr<StaticMesh> sphere = Primitives::Sphere(1.0f, 16, 8);
    REQUIRE(sphere);
    CHECK(sphere->IndexCount() == 16 * 8 * 6);
    // every surface point is ~radius from the origin
    for (const StaticMeshVertex& v : sphere->vertices)
    {
        CHECK(Length(v.position) == doctest::Approx(1.0f).epsilon(0.01));
    }

    RefPtr<StaticMesh> plane = Primitives::Plane(4.0f, 4.0f, 2, 2);
    REQUIRE(plane);
    CHECK(plane->VertexCount() == 9); // (2+1) x (2+1)
    CHECK(plane->IndexCount() == 2 * 2 * 6);
}

TEST_CASE("primitives: cylinder + cone + torus are well-formed (Sedulous ports)")
{
    RefPtr<StaticMesh> cyl = Primitives::Cylinder(0.5f, 2.0f, 16);
    REQUIRE(cyl);
    CHECK(cyl->VertexCount() == 1 + 16 + 1 + 16 + (16 + 1) * 2);
    CHECK(cyl->IndexCount() == 16 * 3 * 2 + 16 * 6);
    CHECK(cyl->bounds.min.y == doctest::Approx(-1.0f));
    CHECK(cyl->bounds.max.y == doctest::Approx(1.0f));
    CHECK(cyl->bounds.max.x == doctest::Approx(0.5f));
    // every vertex sits either on a cap plane or the wall radius
    for (const StaticMeshVertex& v : cyl->vertices)
    {
        const f32 r = Length(Float3{v.position.x, 0.0f, v.position.z});
        CHECK((r < 0.5f + 0.001f));
        CHECK(Abs(v.position.y) == doctest::Approx(1.0f));
    }

    RefPtr<StaticMesh> cone = Primitives::Cone(0.5f, 1.0f, 16);
    REQUIRE(cone);
    CHECK(cone->VertexCount() == 1 + 16 + 1 + 16);
    CHECK(cone->IndexCount() == 16 * 6);
    CHECK(cone->bounds.max.y == doctest::Approx(0.5f));
    CHECK(cone->bounds.min.y == doctest::Approx(-0.5f));

    RefPtr<StaticMesh> torus = Primitives::Torus(1.0f, 0.25f, 16, 8);
    REQUIRE(torus);
    CHECK(torus->VertexCount() == (16 + 1) * (8 + 1));
    CHECK(torus->IndexCount() == 16 * 8 * 6);
    // every surface point is tubeRadius from its ring center
    for (const StaticMeshVertex& v : torus->vertices)
    {
        const Float3 onRing = Normalized(Float3{v.position.x, 0.0f, v.position.z});
        const Float3 center = onRing * 1.0f;
        CHECK(Length(v.position - center) == doctest::Approx(0.25f).epsilon(0.01));
    }
}

TEST_CASE("clear-for-reload empties in place (skinned clears both streams)")
{
    RefPtr<SkinnedMesh> mesh = MakeRef<SkinnedMesh>(DefaultAllocator());
    mesh->vertices.PushBack(StaticMeshVertex{});
    mesh->skinning.PushBack(VertexSkinning{});
    mesh->skeletonIndex = 5;

    mesh->ClearForReload();
    CHECK(mesh->VertexCount() == 0);
    CHECK(mesh->SkinningStream().Size() == 0);
    CHECK(mesh->skeletonIndex == -1);
}

TEST_CASE("tangent generation: mirrored UVs produce handedness w = -1")
{
    // Two triangles with identical geometry; the second's UVs are U-mirrored. The bitangent
    // accumulation opposes cross(N, T) there, so tangent.w flips - the shader's TBN then
    // lights normal maps correctly on mirrored halves.
    RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
    auto addTri = [&](f32 xBase, bool mirrored)
    {
        const u32 base = mesh->VertexCount();
        const f32 u0 = mirrored ? 1.0f : 0.0f;
        const f32 u1 = mirrored ? 0.0f : 1.0f;
        mesh->vertices.PushBack(StaticMeshVertex{Float3{xBase, 0, 0}, Float3{0, 0, 1},
                                                 Float2{u0, 0}, 0xFFFFFFFFu, Float3{1, 0, 0}});
        mesh->vertices.PushBack(StaticMeshVertex{Float3{xBase + 1.0f, 0, 0}, Float3{0, 0, 1},
                                                 Float2{u1, 0}, 0xFFFFFFFFu, Float3{1, 0, 0}});
        mesh->vertices.PushBack(StaticMeshVertex{Float3{xBase, 1, 0}, Float3{0, 0, 1},
                                                 Float2{u0, 1}, 0xFFFFFFFFu, Float3{1, 0, 0}});
        mesh->indices.AddTriangle(base + 0, base + 1, base + 2);
    };
    mesh->indices.Resize(6);
    addTri(0.0f, /*mirrored*/ false);
    addTri(2.0f, /*mirrored*/ true);
    mesh->subMeshes.PushBack(SubMesh{0, 6, 0, PrimitiveType::Triangles});

    mesh->GenerateTangents();
    for (u32 i = 0; i < 3; ++i)
    {
        CHECK(mesh->vertices[i].tangent.w == 1.0f);
    }
    for (u32 i = 3; i < 6; ++i)
    {
        CHECK(mesh->vertices[i].tangent.w == -1.0f);
    }
    // The mirrored tangent points the other way in X; xyz stays unit length.
    CHECK(mesh->vertices[0].tangent.x == doctest::Approx(1.0f));
    CHECK(mesh->vertices[3].tangent.x == doctest::Approx(-1.0f));
}
