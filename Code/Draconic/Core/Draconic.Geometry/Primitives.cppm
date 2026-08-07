/// Draconic::Geometry - the `:primitives` partition.
///
/// Procedural primitive meshes (debug shapes / placeholders / tests). Each returns a
/// fully-formed StaticMesh - static vertex stream + 32-bit indices + one submesh +
/// generated tangents + bounds. These build the typed StaticMesh directly (no generic
/// untyped vertex-buffer indirection).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.geometry:primitives;

import draconic.foundation;
import :types;
import :mesh;

using namespace draconic::foundation;

export namespace draconic::geometry
{

    class Primitives
    {
    public:
        // A unit quad in the XY plane (two triangles), facing +Z.
        [[nodiscard]] static RefPtr<StaticMesh> Quad(f32 width = 1.0f, f32 height = 1.0f)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const f32 hw = width * 0.5f, hh = height * 0.5f;
            const u32 white = 0xFFFFFFFFu;
            mesh->vertices.PushBack(StaticMeshVertex{Float3{-hw, -hh, 0}, Float3{0, 0, 1},
                                                     Float2{0, 1}, white, Float3{1, 0, 0}});
            mesh->vertices.PushBack(StaticMeshVertex{Float3{hw, -hh, 0}, Float3{0, 0, 1},
                                                     Float2{1, 1}, white, Float3{1, 0, 0}});
            mesh->vertices.PushBack(StaticMeshVertex{Float3{hw, hh, 0}, Float3{0, 0, 1},
                                                     Float2{1, 0}, white, Float3{1, 0, 0}});
            mesh->vertices.PushBack(StaticMeshVertex{Float3{-hw, hh, 0}, Float3{0, 0, 1},
                                                     Float2{0, 0}, white, Float3{1, 0, 0}});
            const u32 quad[6] = {0, 1, 2, 0, 2, 3};
            mesh->indices.Resize(6);
            for (u32 i : quad)
            {
                mesh->indices.Add(i);
            }
            Finish(*mesh);
            return mesh;
        }

        // An axis-aligned cube of edge `size`, 24 verts (hard per-face normals).
        [[nodiscard]] static RefPtr<StaticMesh> Cube(f32 size = 1.0f)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const f32 h = size * 0.5f;
            mesh->indices.Resize(36); // 6 faces x 2 triangles x 3 indices
            // 6 faces: (origin corner, edge-u, edge-v, normal)
            AddFace(*mesh, Float3{-h, -h, h}, Float3{1, 0, 0}, Float3{0, 1, 0}, Float3{0, 0, 1},
                    size); // +Z
            AddFace(*mesh, Float3{h, -h, -h}, Float3{-1, 0, 0}, Float3{0, 1, 0}, Float3{0, 0, -1},
                    size); // -Z
            AddFace(*mesh, Float3{h, -h, h}, Float3{0, 0, -1}, Float3{0, 1, 0}, Float3{1, 0, 0},
                    size); // +X
            AddFace(*mesh, Float3{-h, -h, -h}, Float3{0, 0, 1}, Float3{0, 1, 0}, Float3{-1, 0, 0},
                    size); // -X
            AddFace(*mesh, Float3{-h, h, h}, Float3{1, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0},
                    size); // +Y
            AddFace(*mesh, Float3{-h, -h, -h}, Float3{1, 0, 0}, Float3{0, 0, 1}, Float3{0, -1, 0},
                    size); // -Y
            Finish(*mesh);
            return mesh;
        }

        // A flat grid in the XZ plane, `width` x `depth`, subdivided.
        [[nodiscard]] static RefPtr<StaticMesh> Plane(f32 width = 1.0f, f32 depth = 1.0f,
                                                      u32 xSegments = 1, u32 zSegments = 1)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const u32 xs = xSegments < 1 ? 1 : xSegments, zs = zSegments < 1 ? 1 : zSegments;
            const u32 white = 0xFFFFFFFFu;
            for (u32 z = 0; z <= zs; ++z)
            {
                for (u32 x = 0; x <= xs; ++x)
                {
                    const f32 u = static_cast<f32>(x) / static_cast<f32>(xs);
                    const f32 v = static_cast<f32>(z) / static_cast<f32>(zs);
                    mesh->vertices.PushBack(
                        StaticMeshVertex{Float3{(u - 0.5f) * width, 0.0f, (v - 0.5f) * depth},
                                         Float3{0, 1, 0}, Float2{u, v}, white, Float3{1, 0, 0}});
                }
            }
            mesh->indices.Resize(xs * zs * 6);
            const u32 rowStride = xs + 1;
            for (u32 z = 0; z < zs; ++z)
            {
                for (u32 x = 0; x < xs; ++x)
                {
                    const u32 i0 = z * rowStride + x, i1 = i0 + 1, i2 = i0 + rowStride, i3 = i2 + 1;
                    mesh->indices.AddTriangle(i0, i2, i1);
                    mesh->indices.AddTriangle(i1, i2, i3);
                }
            }
            Finish(*mesh);
            return mesh;
        }

        // A UV sphere of `radius` with `segments` longitudes and `rings` latitudes.
        [[nodiscard]] static RefPtr<StaticMesh> Sphere(f32 radius = 0.5f, u32 segments = 32,
                                                       u32 rings = 16)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const u32 seg = segments < 3 ? 3 : segments, rng = rings < 2 ? 2 : rings;
            const u32 white = 0xFFFFFFFFu;
            for (u32 r = 0; r <= rng; ++r)
            {
                const f32 v = static_cast<f32>(r) / static_cast<f32>(rng);
                const f32 phi = v * kPi; // 0..pi (pole to pole)
                const f32 sinPhi = Sin(phi), cosPhi = Cos(phi);
                for (u32 s = 0; s <= seg; ++s)
                {
                    const f32 u = static_cast<f32>(s) / static_cast<f32>(seg);
                    const f32 theta = u * 2.0f * kPi;
                    const Float3 n{Cos(theta) * sinPhi, cosPhi, Sin(theta) * sinPhi};
                    mesh->vertices.PushBack(
                        StaticMeshVertex{n * radius, n, Float2{u, v}, white, Float3{1, 0, 0}});
                }
            }
            mesh->indices.Resize(seg * rng * 6);
            const u32 rowStride = seg + 1;
            for (u32 r = 0; r < rng; ++r)
            {
                for (u32 s = 0; s < seg; ++s)
                {
                    const u32 i0 = r * rowStride + s, i1 = i0 + 1, i2 = i0 + rowStride, i3 = i2 + 1;
                    // CCW-from-outside winding (matches SedulousEngine CreateSphere: (a,b,c),(b,d,c) with
                    // a=i0,b=i1,c=i2,d=i3). The port had (i0,i2,i1)/(i1,i2,i3) - last two swapped - which
                    // reversed the front face to inward, so back-face culling hid the outer shell.
                    mesh->indices.AddTriangle(i0, i1, i2);
                    mesh->indices.AddTriangle(i1, i3, i2);
                }
            }
            Finish(*mesh);
            return mesh;
        }

        // A capped cylinder of `radius` x `height` about the Y axis. Ported from SedulousEngine
        // MeshBuilder.CreateCylinder: caps have hard axial normals (own vertex rings), the side wall
        // has radial normals (duplicated seam column at u=0/1 for clean UVs).
        [[nodiscard]] static RefPtr<StaticMesh> Cylinder(f32 radius = 0.5f, f32 height = 1.0f,
                                                         u32 segments = 32)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const u32 seg = segments < 3 ? 3 : segments;
            const u32 white = 0xFFFFFFFFu;
            const f32 hh = height * 0.5f;

            // Top center + top cap ring.
            const u32 topCenter = 0;
            mesh->vertices.PushBack(StaticMeshVertex{Float3{0, hh, 0}, Float3{0, 1, 0},
                                                     Float2{0.5f, 0.5f}, white, Float3{1, 0, 0}});
            const u32 topRing = mesh->VertexCount();
            for (u32 i = 0; i < seg; ++i)
            {
                const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(seg);
                const f32 x = Cos(a) * radius, z = Sin(a) * radius;
                mesh->vertices.PushBack(
                    StaticMeshVertex{Float3{x, hh, z}, Float3{0, 1, 0},
                                     Float2{x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f},
                                     white, Float3{1, 0, 0}});
            }
            // Bottom center + bottom cap ring.
            const u32 bottomCenter = mesh->VertexCount();
            mesh->vertices.PushBack(StaticMeshVertex{Float3{0, -hh, 0}, Float3{0, -1, 0},
                                                     Float2{0.5f, 0.5f}, white, Float3{1, 0, 0}});
            const u32 bottomRing = mesh->VertexCount();
            for (u32 i = 0; i < seg; ++i)
            {
                const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(seg);
                const f32 x = Cos(a) * radius, z = Sin(a) * radius;
                mesh->vertices.PushBack(
                    StaticMeshVertex{Float3{x, -hh, z}, Float3{0, -1, 0},
                                     Float2{x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f},
                                     white, Float3{1, 0, 0}});
            }
            // Side wall: seg+1 columns (seam duplicated), 2 verts each (top, bottom), radial normals.
            const u32 sideStart = mesh->VertexCount();
            for (u32 i = 0; i <= seg; ++i)
            {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(seg);
                const f32 a = 2.0f * kPi * u;
                const f32 x = Cos(a) * radius, z = Sin(a) * radius;
                const Float3 n = Normalized(Float3{x, 0, z});
                mesh->vertices.PushBack(
                    StaticMeshVertex{Float3{x, hh, z}, n, Float2{u, 0}, white, Float3{1, 0, 0}});
                mesh->vertices.PushBack(
                    StaticMeshVertex{Float3{x, -hh, z}, n, Float2{u, 1}, white, Float3{1, 0, 0}});
            }

            mesh->indices.Resize(seg * 3 * 2 + seg * 6);
            for (u32 i = 0; i < seg; ++i)
            { // top cap (CCW from above)
                mesh->indices.AddTriangle(topCenter, topRing + (i + 1) % seg, topRing + i);
            }
            for (u32 i = 0; i < seg; ++i)
            { // bottom cap (CCW from below)
                mesh->indices.AddTriangle(bottomCenter, bottomRing + i, bottomRing + (i + 1) % seg);
            }
            for (u32 i = 0; i < seg; ++i)
            { // sides
                const u32 topLeft = sideStart + i * 2, bottomLeft = topLeft + 1;
                const u32 topRight = topLeft + 2, bottomRight = topRight + 1;
                mesh->indices.AddTriangle(topLeft, topRight, bottomLeft);
                mesh->indices.AddTriangle(topRight, bottomRight, bottomLeft);
            }
            Finish(*mesh);
            return mesh;
        }

        // A cone of `radius` x `height` about the Y axis (apex at +Y). Ported from SedulousEngine
        // MeshBuilder.CreateCone (slanted side normals; separate flat-normal ring for the base cap).
        [[nodiscard]] static RefPtr<StaticMesh> Cone(f32 radius = 0.5f, f32 height = 1.0f,
                                                     u32 segments = 32)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const u32 seg = segments < 3 ? 3 : segments;
            const u32 white = 0xFFFFFFFFu;
            const f32 hh = height * 0.5f;

            // Tip.
            mesh->vertices.PushBack(StaticMeshVertex{Float3{0, hh, 0}, Float3{0, 1, 0},
                                                     Float2{0.5f, 0}, white, Float3{1, 0, 0}});
            // Base ring for the slanted sides.
            for (u32 i = 0; i < seg; ++i)
            {
                const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(seg);
                const f32 x = Cos(a) * radius, z = Sin(a) * radius;
                const Float3 n = Normalized(Float3{x, radius, z});
                mesh->vertices.PushBack(StaticMeshVertex{
                    Float3{x, -hh, z}, n, Float2{static_cast<f32>(i) / static_cast<f32>(seg), 1},
                    white, Float3{1, 0, 0}});
            }
            // Base center + flat-normal base ring.
            const u32 baseCenter = mesh->VertexCount();
            mesh->vertices.PushBack(StaticMeshVertex{Float3{0, -hh, 0}, Float3{0, -1, 0},
                                                     Float2{0.5f, 0.5f}, white, Float3{1, 0, 0}});
            for (u32 i = 0; i < seg; ++i)
            {
                const f32 a = 2.0f * kPi * static_cast<f32>(i) / static_cast<f32>(seg);
                const f32 x = Cos(a) * radius, z = Sin(a) * radius;
                mesh->vertices.PushBack(
                    StaticMeshVertex{Float3{x, -hh, z}, Float3{0, -1, 0},
                                     Float2{x / radius * 0.5f + 0.5f, z / radius * 0.5f + 0.5f},
                                     white, Float3{1, 0, 0}});
            }

            mesh->indices.Resize(seg * 6);
            for (u32 i = 0; i < seg; ++i)
            { // sides
                mesh->indices.AddTriangle(0, 1 + (i + 1) % seg, 1 + i);
            }
            for (u32 i = 0; i < seg; ++i)
            { // base
                mesh->indices.AddTriangle(baseCenter, baseCenter + 1 + i,
                                          baseCenter + 1 + (i + 1) % seg);
            }
            Finish(*mesh);
            return mesh;
        }

        // A torus about the Y axis: ring `radius`, tube `tubeRadius`. Ported from SedulousEngine
        // MeshBuilder.CreateTorus (same (a,b,c)/(b,d,c) outward winding as Sphere).
        [[nodiscard]] static RefPtr<StaticMesh> Torus(f32 radius = 1.0f, f32 tubeRadius = 0.3f,
                                                      u32 segments = 32, u32 tubeSegments = 16)
        {
            RefPtr<StaticMesh> mesh = MakeRef<StaticMesh>(DefaultAllocator());
            const u32 seg = segments < 3 ? 3 : segments, tseg = tubeSegments < 3 ? 3 : tubeSegments;
            const u32 white = 0xFFFFFFFFu;
            for (u32 i = 0; i <= seg; ++i)
            {
                const f32 u = static_cast<f32>(i) / static_cast<f32>(seg);
                const f32 theta = u * 2.0f * kPi;
                const f32 ct = Cos(theta), st = Sin(theta);
                for (u32 j = 0; j <= tseg; ++j)
                {
                    const f32 v = static_cast<f32>(j) / static_cast<f32>(tseg);
                    const f32 phi = v * 2.0f * kPi;
                    const f32 cp = Cos(phi), sp = Sin(phi);
                    const Float3 pos{(radius + tubeRadius * cp) * ct, tubeRadius * sp,
                                     (radius + tubeRadius * cp) * st};
                    const Float3 center{radius * ct, 0, radius * st};
                    mesh->vertices.PushBack(StaticMeshVertex{pos, Normalized(pos - center),
                                                             Float2{u, v}, white, Float3{1, 0, 0}});
                }
            }
            mesh->indices.Resize(seg * tseg * 6);
            const u32 rowStride = tseg + 1;
            for (u32 i = 0; i < seg; ++i)
            {
                for (u32 j = 0; j < tseg; ++j)
                {
                    const u32 a = i * rowStride + j, b = a + 1, c = a + rowStride, d = c + 1;
                    mesh->indices.AddTriangle(a, b, c);
                    mesh->indices.AddTriangle(b, d, c);
                }
            }
            Finish(*mesh);
            return mesh;
        }

    private:
        // Adds a quad face (4 verts, 2 tris) anchored at `origin`, spanning `size` along
        // unit edges `eu`/`ev`, with face normal `n`. The caller pre-sizes the index
        // buffer; indices append through its cursor.
        static void AddFace(StaticMesh& mesh, Float3 origin, Float3 eu, Float3 ev, Float3 n,
                            f32 size)
        {
            const u32 base = mesh.VertexCount();
            const u32 white = 0xFFFFFFFFu;
            const Float3 u = eu * size, v = ev * size;
            mesh.vertices.PushBack(
                StaticMeshVertex{origin, n, Float2{0, 1}, white, Float3{1, 0, 0}});
            mesh.vertices.PushBack(
                StaticMeshVertex{origin + u, n, Float2{1, 1}, white, Float3{1, 0, 0}});
            mesh.vertices.PushBack(
                StaticMeshVertex{origin + u + v, n, Float2{1, 0}, white, Float3{1, 0, 0}});
            mesh.vertices.PushBack(
                StaticMeshVertex{origin + v, n, Float2{0, 0}, white, Float3{1, 0, 0}});
            mesh.indices.AddTriangle(base, base + 1, base + 2);
            mesh.indices.AddTriangle(base, base + 2, base + 3);
        }

        static void Finish(StaticMesh& mesh)
        {
            mesh.GenerateTangents();
            mesh.CalculateBounds();
            mesh.subMeshes.PushBack(
                SubMesh{0, static_cast<i32>(mesh.IndexCount()), 0, PrimitiveType::Triangles});
        }
    };

} // namespace draconic::geometry
