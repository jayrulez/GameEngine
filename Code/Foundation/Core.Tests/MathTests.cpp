// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

#include <doctest/doctest.h>

#include <cstring>

#include "Core/Debug/Assert.h"
#include "Core/Log/Log.h"
#include "Core/Reflection/Reflect.h"

import foundation.core;

using namespace foundation::core;

// --- Math: scalars ---------------------------------------------------------

TEST_CASE("math: scalar helpers")
{
    CHECK(Abs(-3.0f) == 3.0f);
    CHECK(NearlyEqual(DegreesToRadians(180.0f), kPi));
    CHECK(NearlyEqual(RadiansToDegrees(kPi), 180.0f));
    CHECK(NearlyEqual(Lerp(0.0f, 10.0f, 0.25f), 2.5f));
    CHECK(NearlyEqual(Sqrt(16.0f), 4.0f));
    CHECK(NearlyZero(1.0e-8f));
    CHECK(Floor(3.7f) == 3.0f);
    CHECK(Ceil(3.2f) == 4.0f);
    CHECK(Round(3.3f) == 3.0f);
    CHECK(Round(3.5f) == 4.0f);
    CHECK(Round(-3.5f) == -4.0f);

    static_assert(Abs(-1.0f) == 1.0f);
}

// --- Math: Float3 ------------------------------------------------------------

TEST_CASE("math: Float3 arithmetic")
{
    Float3 a{1.0f, 2.0f, 3.0f};
    Float3 b{4.0f, 5.0f, 6.0f};

    CHECK((a + b) == Float3{5.0f, 7.0f, 9.0f});
    CHECK((b - a) == Float3{3.0f, 3.0f, 3.0f});
    CHECK((a * 2.0f) == Float3{2.0f, 4.0f, 6.0f});
    CHECK((2.0f * a) == Float3{2.0f, 4.0f, 6.0f});
    CHECK((-a) == Float3{-1.0f, -2.0f, -3.0f});

    a += b;
    CHECK(a == Float3{5.0f, 7.0f, 9.0f});

    CHECK(a[0] == 5.0f);
    CHECK(a[2] == 9.0f);

    static_assert(Float3{1.0f, 0.0f, 0.0f} == Float3::UnitX);
}

TEST_CASE("math: Float3 dot, cross, length, normalize")
{
    CHECK(Dot(Float3{1.0f, 2.0f, 3.0f}, Float3{4.0f, 5.0f, 6.0f}) == 32.0f);

    // Right-handed cross: X x Y = Z
    CHECK(Cross(Float3::UnitX, Float3::UnitY) == Float3::UnitZ);
    CHECK(Cross(Float3::UnitY, Float3::UnitZ) == Float3::UnitX);

    CHECK(LengthSquared(Float3{3.0f, 4.0f, 0.0f}) == 25.0f);
    CHECK(NearlyEqual(Length(Float3{3.0f, 4.0f, 0.0f}), 5.0f));

    Float3 n = Normalized(Float3{0.0f, 8.0f, 0.0f});
    CHECK(NearlyEqual(n, Float3::UnitY));
    CHECK(NearlyEqual(Length(n), 1.0f));

    // Degenerate input -> Zero, no NaN/divide-by-zero.
    CHECK(Normalized(Float3::Zero) == Float3::Zero);
}

TEST_CASE("math: Float3 lerp/min/max")
{
    CHECK(Lerp(Float3::Zero, Float3{4.0f, 8.0f, 12.0f}, 0.5f) == Float3{2.0f, 4.0f, 6.0f});
    CHECK(Min(Float3{1.0f, 5.0f, 3.0f}, Float3{4.0f, 2.0f, 6.0f}) == Float3{1.0f, 2.0f, 3.0f});
    CHECK(Max(Float3{1.0f, 5.0f, 3.0f}, Float3{4.0f, 2.0f, 6.0f}) == Float3{4.0f, 5.0f, 6.0f});
}

// --- Math: Float2 / Float4 -----------------------------------------------------

TEST_CASE("math: Float2 and Float4 basics")
{
    CHECK(Dot(Float2{1.0f, 2.0f}, Float2{3.0f, 4.0f}) == 11.0f);
    CHECK(NearlyEqual(Length(Float2{3.0f, 4.0f}), 5.0f));

    Float4 v{Float3{1.0f, 2.0f, 3.0f}, 1.0f};
    CHECK(v.XYZ() == Float3{1.0f, 2.0f, 3.0f});
    CHECK(v.w == 1.0f);
    CHECK(Dot(Float4::One, Float4::One) == 4.0f);
}

// --- Math: Float4x4 ------------------------------------------------------------

TEST_CASE("math: Float4x4 identity and multiply")
{
    const Float4x4 id = Float4x4::Identity();
    const Float4x4 t = Float4x4::Translation(Float3{1.0f, 2.0f, 3.0f});

    CHECK(NearlyEqual(id * t, t));
    CHECK(NearlyEqual(t * id, t));

    static_assert(Float4x4::Identity()(0, 0) == 1.0f);
    static_assert(Float4x4::Identity()(0, 1) == 0.0f);
}

TEST_CASE("math: Float4x4 translation lives in the last row (row vectors)")
{
    const Float4x4 t = Float4x4::Translation(Float3{10.0f, 20.0f, 30.0f});
    CHECK(t.m[3][0] == 10.0f);
    CHECK(t.m[3][1] == 20.0f);
    CHECK(t.m[3][2] == 30.0f);

    const Float3 p = TransformPoint(Float3{1.0f, 1.0f, 1.0f}, t);
    CHECK(NearlyEqual(p, Float3{11.0f, 21.0f, 31.0f}));

    // Directions ignore translation.
    CHECK(NearlyEqual(TransformDirection(Float3{1.0f, 0.0f, 0.0f}, t), Float3{1.0f, 0.0f, 0.0f}));
}

TEST_CASE("math: Float4x4 rotation (row vectors): RotationZ(90) maps +X to +Y")
{
    const Float4x4 rz = Float4x4::RotationZ(DegreesToRadians(90.0f));
    CHECK(NearlyEqual(TransformDirection(Float3::UnitX, rz), Float3::UnitY));

    const Float4x4 ry = Float4x4::RotationY(DegreesToRadians(90.0f));
    // RotationY(90) maps +Z to +X.
    CHECK(NearlyEqual(TransformDirection(Float3::UnitZ, ry), Float3::UnitX));
}

TEST_CASE("math: Float4x4 2D affine helpers (TransformPoint2D, operator==)")
{
    CHECK(Float4x4::Identity() == Float4x4::Identity());
    CHECK_FALSE(Float4x4::Translation(Float3{1, 0, 0}) == Float4x4::Identity());

    // 2D translate.
    const Float4x4 t = Float4x4::Translation(Float3{5.0f, 7.0f, 0.0f});
    CHECK(NearlyEqual(TransformPoint2D(Float2{1.0f, 2.0f}, t), Float2{6.0f, 9.0f}));

    // 2D scale-then-translate (row vectors, left-to-right): v * (S * T).
    const Float4x4 st =
        Float4x4::Scale(Float3{2.0f, 3.0f, 1.0f}) * Float4x4::Translation(Float3{1.0f, 1.0f, 0.0f});
    CHECK(NearlyEqual(TransformPoint2D(Float2{1.0f, 1.0f}, st), Float2{3.0f, 4.0f})); // (2,3)+(1,1)

    // RotationZ(90) maps +X to +Y in 2D too.
    const Float4x4 rz = Float4x4::RotationZ(DegreesToRadians(90.0f));
    CHECK(NearlyEqual(TransformPoint2D(Float2{1.0f, 0.0f}, rz), Float2{0.0f, 1.0f}));
}

TEST_CASE("math: Float4x4 composition reads left-to-right (scale then translate)")
{
    // v * (S * T): scale first, then translate.
    const Float4x4 st =
        Float4x4::Scale(Float3{2.0f, 2.0f, 2.0f}) * Float4x4::Translation(Float3{1.0f, 0.0f, 0.0f});
    const Float3 p = TransformPoint(Float3{1.0f, 1.0f, 1.0f}, st);
    CHECK(NearlyEqual(p, Float3{3.0f, 2.0f, 2.0f})); // (2,2,2) + (1,0,0)
}

TEST_CASE("math: perspective has the expected projective structure")
{
    const Float4x4 proj = Float4x4::PerspectiveFovRH(DegreesToRadians(90.0f), 1.0f, 1.0f, 100.0f);
    CHECK(proj.m[2][3] == -1.0f);           // w' = -z (RH)
    CHECK(NearlyEqual(proj.m[0][0], 1.0f)); // xScale = 1/tan(45) at aspect 1
}

namespace
{
    // NDC depth of a point `distance` in front of a camera at the origin looking down -Z.
    f32 NdcDepthAt(const Float4x4& proj, f32 distance)
    {
        const Float4 clip = Float4{0.0f, 0.0f, -distance, 1.0f} * proj;
        return clip.z / clip.w;
    }
}

TEST_CASE("math: the depth convention is reverse-Z - near maps to 1, far to 0, nearer is larger")
{
    static_assert(projection::kReverseZ);
    static_assert(projection::kNdcDepthNear == 1.0f);
    static_assert(projection::kNdcDepthFar == 0.0f);
    static_assert(projection::IsNearer(0.7f, 0.2f));
    static_assert(projection::IsBackground(projection::kNdcDepthFar));
    static_assert(!projection::IsBackground(1.0e-7f));

    const f32 n = 0.1f, f = 1000.0f;
    const Float4x4 proj = Float4x4::PerspectiveFovRH(1.0f, 16.0f / 9.0f, n, f);
    CHECK(NearlyEqual(NdcDepthAt(proj, n), projection::kNdcDepthNear, 1.0e-5f));
    CHECK(NearlyEqual(NdcDepthAt(proj, f), projection::kNdcDepthFar, 1.0e-5f));
    // Monotonic: farther is smaller, and the matrix agrees with the closed form.
    f32 previous = 2.0f;
    const f32 distances[] = {n, 0.5f, 1.0f, 10.0f, 100.0f, 500.0f, f};
    for (f32 d : distances)
    {
        const f32 depth = NdcDepthAt(proj, d);
        CHECK(depth < previous);
        CHECK(NearlyEqual(depth, projection::DepthAtDistance(d, n, f), 1.0e-5f));
        // Linearize inverts it (relative tolerance: the far end is a large number).
        CHECK(Abs(projection::LinearizeDepth(depth, n, f) - d) <= d * 1.0e-4f);
        previous = depth;
    }
    // Reverse-Z spends the float where a perspective divide starves it: two surfaces 0.02 apart at
    // 900 units are ~2500 float ulps apart (they z-fight under standard-Z, where the gap is a
    // fraction of an ulp at depth ~1).
    const f32 wall = NdcDepthAt(proj, 900.0f);
    const f32 face = NdcDepthAt(proj, 900.0f - 0.02f);
    CHECK(face > wall);
    CHECK((face - wall) > 100.0f * 1.2e-7f * wall); // > 100 ulps of the smaller value

    // Orthographic: the same reading.
    const Float4x4 ortho = Float4x4::OrthographicRH(10.0f, 10.0f, 2.0f, 50.0f);
    CHECK(NearlyEqual(NdcDepthAt(ortho, 2.0f), projection::kNdcDepthNear, 1.0e-6f));
    CHECK(NearlyEqual(NdcDepthAt(ortho, 50.0f), projection::kNdcDepthFar, 1.0e-6f));
    CHECK(NearlyEqual(NdcDepthAt(ortho, 26.0f), 0.5f, 1.0e-6f));
}

TEST_CASE("math: IsOrthographic tells an orthographic projection from a perspective one")
{
    CHECK(projection::IsOrthographic(Float4x4::OrthographicRH(40.0f, 22.5f, 0.1f, 100.0f)));
    CHECK_FALSE(
        projection::IsOrthographic(Float4x4::PerspectiveFovRH(1.0f, 16.0f / 9.0f, 0.1f, 100.0f)));
}

TEST_CASE("math: a projection's near and far planes read back from it, jittered or not")
{
    Float4x4 perspective = Float4x4::PerspectiveFovRH(1.0f, 16.0f / 9.0f, 0.3f, 250.0f);
    CHECK(NearlyEqual(projection::NearPlane(perspective), 0.3f, 1.0e-4f));
    CHECK(NearlyEqual(projection::FarPlane(perspective), 250.0f, 1.0e-2f));
    Float4x4 ortho = Float4x4::OrthographicRH(40.0f, 22.5f, -50.0f, 120.0f);
    CHECK(NearlyEqual(projection::NearPlane(ortho), -50.0f, 1.0e-3f));
    CHECK(NearlyEqual(projection::FarPlane(ortho), 120.0f, 1.0e-3f));

    // The TAA jitter sits in row 2 (perspective) or row 3 (orthographic), x and y: not the planes.
    perspective.m[2][0] += 0.0007f;
    perspective.m[2][1] -= 0.0004f;
    ortho.m[3][0] += 0.0007f;
    ortho.m[3][1] -= 0.0004f;
    CHECK(NearlyEqual(projection::NearPlane(perspective), 0.3f, 1.0e-4f));
    CHECK(NearlyEqual(projection::NearPlane(ortho), -50.0f, 1.0e-3f));
}

TEST_CASE("math: an orthographic depth linearizes back to its distance")
{
    const f32 n = -20.0f, f = 80.0f;
    const Float4x4 ortho = Float4x4::OrthographicRH(10.0f, 10.0f, n, f);
    const f32 distances[] = {-20.0f, -3.5f, 0.0f, 12.0f, 79.0f};
    for (const f32 d : distances)
    {
        CHECK(NearlyEqual(projection::LinearizeDepthOrthographic(NdcDepthAt(ortho, d), n, f), d,
                          1.0e-3f));
    }
}

TEST_CASE("math: the TAA jitter shifts NDC by the same amount at every depth, either projection")
{
    const auto ndcXY = [](const Float4x4& proj, f32 x, f32 y, f32 distance)
    {
        const Float4 clip = Float4{x, y, -distance, 1.0f} * proj;
        return Float2{clip.x / clip.w, clip.y / clip.w};
    };
    const f32 jx = 0.0031f, jy = -0.0017f;
    const Float4x4 projections[] = {Float4x4::PerspectiveFovRH(1.0f, 1.5f, 0.1f, 500.0f),
                                    Float4x4::OrthographicRH(20.0f, 12.0f, 0.1f, 500.0f)};
    const f32 depths[] = {1.0f, 40.0f, 300.0f};
    for (const Float4x4& plain : projections)
    {
        Float4x4 jittered = plain;
        projection::ApplyJitter(jittered, jx, jy);
        CHECK(NearlyEqual(projection::JitterOf(jittered).x, jx, 1.0e-7f));
        CHECK(NearlyEqual(projection::JitterOf(jittered).y, jy, 1.0e-7f));
        CHECK(NearlyEqual(projection::NearPlane(jittered), projection::NearPlane(plain), 1.0e-4f));
        for (const f32 distance : depths)
        {
            const Float2 a = ndcXY(plain, 0.7f, -0.4f, distance);
            const Float2 b = ndcXY(jittered, 0.7f, -0.4f, distance);
            CHECK(NearlyEqual(b.x - a.x, -jx, 1.0e-5f));
            CHECK(NearlyEqual(b.y - a.y, -jy, 1.0e-5f));
        }
    }
}

TEST_CASE("math: BoundingFrustum extracts the reverse-Z near and far planes where the camera puts them")
{
    const f32 n = 0.5f, f = 200.0f;
    const Float4x4 view = Float4x4::LookAtRH(Float3{0, 0, 0}, Float3{0, 0, -1}, Float3{0, 1, 0});
    const Float4x4 proj = Float4x4::PerspectiveFovRH(DegreesToRadians(90.0f), 1.0f, n, f);
    const BoundingFrustum frustum{view * proj};

    // Just inside each plane is contained; just outside is not.
    CHECK(frustum.Contains(Float3{0, 0, -(n + 0.01f)}) == ContainmentType::Contains);
    CHECK(frustum.Contains(Float3{0, 0, -(n - 0.01f)}) == ContainmentType::Disjoint);
    CHECK(frustum.Contains(Float3{0, 0, -(f - 0.1f)}) == ContainmentType::Contains);
    CHECK(frustum.Contains(Float3{0, 0, -(f + 0.1f)}) == ContainmentType::Disjoint);
    CHECK(frustum.Contains(Float3{0, 0, 1.0f}) == ContainmentType::Disjoint); // behind the camera

    // The named planes sit at the near and far distances, and the corners follow them.
    CHECK(NearlyEqual(-Dot(frustum.Near().normal, Float3{0, 0, -n}) - frustum.Near().d, 0.0f, 1.0e-3f));
    CHECK(NearlyEqual(-Dot(frustum.Far().normal, Float3{0, 0, -f}) - frustum.Far().d, 0.0f, 1.0e-2f));
    for (i32 i = 0; i < 4; ++i)
    {
        CHECK(NearlyEqual(frustum.corners[i].z, -n, 1.0e-3f));
        CHECK(NearlyEqual(frustum.corners[4 + i].z, -f, 1.0e-1f));
    }
    // A sphere straddling the far plane intersects; one past it does not.
    CHECK(Intersects(frustum, BoundingSphere{Float3{0, 0, -f}, 1.0f}));
    CHECK_FALSE(Intersects(frustum, BoundingSphere{Float3{0, 0, -(f + 5.0f)}, 1.0f}));
}

// --- Math: Quaternion ------------------------------------------------------------

TEST_CASE("math: Quaternion rotates vectors and agrees with its matrix")
{
    const Quaternion q = Quaternion::FromAxisAngle(Float3::UnitZ, DegreesToRadians(90.0f));

    // 90 deg about Z maps +X to +Y.
    CHECK(NearlyEqual(RotateVector(q, Float3::UnitX), Float3::UnitY));

    // Quaternion rotation and its matrix agree.
    const Float4x4 r = RotationMatrix(q);
    CHECK(NearlyEqual(RotateVector(q, Float3::UnitX), TransformDirection(Float3::UnitX, r)));
    CHECK(NearlyEqual(RotateVector(q, Float3{0.3f, -0.5f, 0.8f}),
                      TransformDirection(Float3{0.3f, -0.5f, 0.8f}, r)));

    // Identity does nothing.
    CHECK(NearlyEqual(RotateVector(Quaternion::Identity, Float3{1.0f, 2.0f, 3.0f}),
                      Float3{1.0f, 2.0f, 3.0f}));

    // Composition: two 45-deg rotations == one 90-deg.
    const Quaternion half = Quaternion::FromAxisAngle(Float3::UnitZ, DegreesToRadians(45.0f));
    CHECK(NearlyEqual(RotateVector(half * half, Float3::UnitX), Float3::UnitY));
}

// --- Math: Transform -------------------------------------------------------

TEST_CASE("math: Transform composes scale, rotation, translation")
{
    Transform xform;
    xform.scale = Float3{2.0f, 2.0f, 2.0f};
    xform.rotation = Quaternion::FromAxisAngle(Float3::UnitZ, DegreesToRadians(90.0f));
    xform.position = Float3{5.0f, 0.0f, 0.0f};

    const Float4x4 m = xform.ToMatrix();

    // (1,0,0) scaled by 2 is (2,0,0), rotated 90 about Z is (0,2,0), translated is (5,2,0).
    const Float3 p = TransformPoint(Float3::UnitX, m);
    CHECK(NearlyEqual(p, Float3{5.0f, 2.0f, 0.0f}));

    // Identity transform is a no-op.
    Transform identity;
    CHECK(NearlyEqual(TransformPoint(Float3{7.0f, 8.0f, 9.0f}, identity.ToMatrix()),
                      Float3{7.0f, 8.0f, 9.0f}));
}

// --- Math: Float4x4 determinant / inverse --------------------------------------

TEST_CASE("math: Float4x4 determinant")
{
    CHECK(NearlyEqual(Determinant(Float4x4::Identity()), 1.0f));
    CHECK(NearlyEqual(Determinant(Float4x4::Scale(Float3{2.0f, 3.0f, 4.0f})), 24.0f));
}

TEST_CASE("math: Float4x4 inverse undoes the transform")
{
    Transform xform;
    xform.scale = Float3{2.0f, 0.5f, 3.0f};
    xform.rotation =
        Quaternion::FromAxisAngle(Normalized(Float3{1.0f, 2.0f, 3.0f}), DegreesToRadians(50.0f));
    xform.position = Float3{5.0f, -2.0f, 1.0f};

    const Float4x4 m = xform.ToMatrix();
    const Float4x4 inv = Inverse(m);

    CHECK(NearlyEqual(m * inv, Float4x4::Identity(), 1.0e-3f));
    CHECK(NearlyEqual(inv * m, Float4x4::Identity(), 1.0e-3f));

    // A point transformed then inverse-transformed returns to itself.
    const Float3 p{3.0f, 4.0f, 5.0f};
    const Float3 roundTrip = TransformPoint(TransformPoint(p, m), inv);
    CHECK(NearlyEqual(roundTrip, p, 1.0e-3f));

    // Singular matrix -> Identity (no divide-by-zero).
    CHECK(NearlyEqual(Inverse(Float4x4::Scale(Float3::Zero)), Float4x4::Identity()));
}

// --- Math: Quaternion Slerp ------------------------------------------------------

TEST_CASE("math: Quaternion Slerp endpoints and midpoint")
{
    const Quaternion a = Quaternion::Identity;
    const Quaternion b = Quaternion::FromAxisAngle(Float3::UnitZ, DegreesToRadians(90.0f));

    CHECK(NearlyEqual(Slerp(a, b, 0.0f), a));
    CHECK(NearlyEqual(Slerp(a, b, 1.0f), b));

    // Halfway between 0 and 90 deg about Z is 45 deg: maps +X to (cos45, sin45, 0).
    const Quaternion mid = Slerp(a, b, 0.5f);
    const Float3 rotated = RotateVector(mid, Float3::UnitX);
    const f32 c = Cos(DegreesToRadians(45.0f));
    CHECK(NearlyEqual(rotated, Float3{c, c, 0.0f}, 1.0e-4f));
}

// --- Math: Float2 / Float4 normalize -------------------------------------------

TEST_CASE("math: Float2 and Float4 Normalized")
{
    CHECK(NearlyEqual(Length(Normalized(Float2{3.0f, 4.0f})), 1.0f));
    CHECK(Normalized(Float2::Zero) == Float2::Zero);

    CHECK(NearlyEqual(Length(Normalized(Float4{1.0f, 2.0f, 2.0f, 4.0f})), 1.0f));
    CHECK(Normalized(Float4::Zero) == Float4::Zero);
}

// --- Math: Float3x3 ------------------------------------------------------------

TEST_CASE("math: Float3x3 identity, multiply, transpose")
{
    const Float3x3 id = Float3x3::Identity();
    const Float3x3 r = Float3x3::FromMat4(Float4x4::RotationZ(DegreesToRadians(90.0f)));

    CHECK(NearlyEqual(id * r, r));
    CHECK(NearlyEqual(Transpose(Transpose(r)), r));

    static_assert(Float3x3::Identity()(1, 1) == 1.0f);
}

TEST_CASE("math: Float3x3 row-vector rotation matches Float4x4")
{
    const Float3x3 rz = Float3x3::FromMat4(Float4x4::RotationZ(DegreesToRadians(90.0f)));
    CHECK(NearlyEqual(Float3::UnitX * rz, Float3::UnitY));
}

TEST_CASE("math: Float3x3 determinant and inverse")
{
    const Float3x3 rz = Float3x3::FromMat4(Float4x4::RotationZ(DegreesToRadians(37.0f)));
    CHECK(NearlyEqual(Determinant(rz), 1.0f)); // pure rotation

    const Float3x3 inv = Inverse(rz);
    CHECK(NearlyEqual(rz * inv, Float3x3::Identity(), 1.0e-4f));

    // For a rotation, the inverse equals the transpose.
    CHECK(NearlyEqual(inv, Transpose(rz), 1.0e-4f));

    // Singular -> Identity.
    Float3x3 zero{};
    CHECK(NearlyEqual(Inverse(zero), Float3x3::Identity()));
}

// --- Math: geometry --------------------------------------------------------

TEST_CASE("geometry: AABB contains, expand, merge")
{
    AABB box{Float3{0.0f, 0.0f, 0.0f}, Float3{2.0f, 2.0f, 2.0f}};
    CHECK(box.Contains(Float3{1.0f, 1.0f, 1.0f}));
    CHECK_FALSE(box.Contains(Float3{3.0f, 1.0f, 1.0f}));
    CHECK(NearlyEqual(box.Center(), Float3{1.0f, 1.0f, 1.0f}));
    CHECK(NearlyEqual(box.Extents(), Float3{1.0f, 1.0f, 1.0f}));

    // Build from points via Empty + Expand.
    AABB grown = AABB::Empty();
    CHECK_FALSE(grown.IsValid());
    grown.Expand(Float3{-1.0f, 0.0f, 5.0f});
    grown.Expand(Float3{3.0f, 4.0f, -2.0f});
    CHECK(grown.IsValid());
    CHECK(NearlyEqual(grown.min, Float3{-1.0f, 0.0f, -2.0f}));
    CHECK(NearlyEqual(grown.max, Float3{3.0f, 4.0f, 5.0f}));

    AABB a{Float3{0.0f, 0.0f, 0.0f}, Float3{1.0f, 1.0f, 1.0f}};
    AABB b{Float3{2.0f, 2.0f, 2.0f}, Float3{3.0f, 3.0f, 3.0f}};
    CHECK_FALSE(a.Intersects(b));
    AABB m = Merge(a, b);
    CHECK(NearlyEqual(m.min, Float3::Zero));
    CHECK(NearlyEqual(m.max, Float3{3.0f, 3.0f, 3.0f}));
    CHECK(m.Intersects(a));
}

TEST_CASE("geometry: Plane signed distance")
{
    // XZ plane at y = 0, normal +Y.
    const Plane plane = Plane::FromPointNormal(Float3::Zero, Float3::UnitY);
    CHECK(NearlyEqual(plane.SignedDistance(Float3{5.0f, 0.0f, -3.0f}), 0.0f));
    CHECK(NearlyEqual(plane.SignedDistance(Float3{0.0f, 2.0f, 0.0f}), 2.0f));
    CHECK(NearlyEqual(plane.SignedDistance(Float3{0.0f, -4.0f, 0.0f}), -4.0f));

    const Plane unnormalized{Float3{0.0f, 3.0f, 0.0f}, 0.0f};
    CHECK(NearlyEqual(Length(unnormalized.Normalized().normal), 1.0f));
}

TEST_CASE("geometry: Rectangle contains and intersects")
{
    Rectangle r{0.0f, 0.0f, 4.0f, 2.0f};
    CHECK(r.Contains(Float2{2.0f, 1.0f}));
    CHECK_FALSE(r.Contains(Float2{5.0f, 1.0f}));
    CHECK(NearlyEqual(r.Center(), Float2{2.0f, 1.0f}));

    CHECK(r.Intersects(Rectangle{3.0f, 1.0f, 2.0f, 2.0f}));
    CHECK_FALSE(r.Intersects(Rectangle{10.0f, 10.0f, 1.0f, 1.0f}));
}

// --- Math: Color -----------------------------------------------------------

TEST_CASE("color: pack/unpack and operations")
{
    CHECK(Color::White.ToRGBA8() == 0xFFFFFFFFu);
    CHECK(Color::Red.ToRGBA8() == 0xFF0000FFu);
    CHECK(Color::Transparent.ToRGBA8() == 0x00000000u);

    const Color c = Color::FromRGBA8(0xFF0000FFu);
    CHECK(NearlyEqual(c, Color::Red));

    CHECK(NearlyEqual(Lerp(Color::Black, Color::White, 0.5f), Color{0.5f, 0.5f, 0.5f, 1.0f}));
    CHECK(NearlyEqual(Color::White * 0.25f, Color{0.25f, 0.25f, 0.25f, 0.25f}));

    // Round-trip through 8-bit packing (within quantization tolerance).
    const Color original{0.2f, 0.4f, 0.6f, 0.8f};
    CHECK(NearlyEqual(Color::FromRGBA8(original.ToRGBA8()), original, 1.0f / 255.0f));
}

TEST_CASE("color32: packed byte color and conversions")
{
    CHECK(Color32::White.ToRGBA8() == 0xFFFFFFFFu);
    CHECK(Color32::Red.ToRGBA8() == 0xFF0000FFu);
    CHECK(Color32::Transparent.ToRGBA8() == 0x00000000u);
    CHECK(Color32::FromRGBA8(0x10203040u) == Color32{0x10, 0x20, 0x30, 0x40});

    // Float <-> byte conversions, no gamma.
    CHECK(ToColor32(Color::Red) == Color32::Red);
    CHECK(NearlyEqual(ToColor(Color32::Blue), Color::Blue));

    // Every byte value round-trips exactly: Color32 -> Color -> Color32.
    bool exact = true;
    for (u32 v = 0; v <= 255; ++v)
    {
        const Color32 c{static_cast<u8>(v), static_cast<u8>(255 - v), static_cast<u8>(v), 255};
        if (!(ToColor32(ToColor(c)) == c))
        {
            exact = false;
            break;
        }
    }
    CHECK(exact);
}

TEST_CASE("color: an authored (sRGB) colour decodes to linear for the renderer")
{
    // sRGB mid grey (#808080's float) is linear 0.2140; white and black are fixed points.
    const Color grey = ToLinear(Color{0.5f, 0.5f, 0.5f, 0.5f});
    CHECK(grey.r == doctest::Approx(0.2140f).epsilon(0.001));
    CHECK(grey.g == doctest::Approx(0.2140f).epsilon(0.001));
    CHECK(grey.a == doctest::Approx(0.5f)); // alpha is coverage, not decoded
    CHECK(NearlyEqual(ToLinear(Color::White), Color::White, 1.0e-5f));
    CHECK(NearlyEqual(ToLinear(Color::Black), Color::Black));

    // ToSrgb inverts it, so a linear value (a glTF factor) can become an authored one.
    const Color linear{0.05f, 0.3f, 0.9f, 1.0f};
    CHECK(NearlyEqual(ToLinear(ToSrgb(linear)), linear, 1.0e-5f));

    // Over-bright stays over-bright (monotonic past 1).
    CHECK(ToLinear(Color{2.0f, 0.0f, 0.0f, 1.0f}).r > 1.0f);
}

// --- Math: easing functions ---

TEST_CASE("math: easing endpoints + known values")
{
    // Every easing maps 0 -> ~0 and 1 -> ~1 (the interpolation factor endpoints).
    EasingFunction fns[] = {
        EaseInLinear,       EaseOutLinear,     EaseInQuadratic,    EaseOutQuadratic,
        EaseInOutQuadratic, EaseInCubic,       EaseOutCubic,       EaseInOutCubic,
        EaseInQuartic,      EaseOutQuartic,    EaseInOutQuartic,   EaseInQuintic,
        EaseOutQuintic,     EaseInOutQuintic,  EaseInSin,          EaseOutSin,
        EaseInOutSin,       EaseInExponential, EaseOutExponential, EaseInOutExponential,
        EaseInCircular,     EaseOutCircular,   EaseInOutCircular,  EaseInBack,
        EaseOutBack,        EaseInOutBack,     EaseInElastic,      EaseOutElastic,
        EaseInOutElastic,   EaseInBounce,      EaseOutBounce,      EaseInOutBounce,
    };
    for (EasingFunction f : fns)
    {
        CHECK(NearlyEqual(f(0.0f), 0.0f));
        CHECK(NearlyEqual(f(1.0f), 1.0f));
    }

    // Specific known polynomial values.
    CHECK(NearlyEqual(EaseInQuadratic(0.5f), 0.25f));
    CHECK(NearlyEqual(EaseOutQuadratic(0.5f), 0.75f));
    CHECK(NearlyEqual(EaseInOutQuadratic(0.5f), 0.5f));
    CHECK(NearlyEqual(EaseInCubic(0.5f), 0.125f));
    CHECK(NearlyEqual(EaseInOutCubic(0.5f), 0.5f));
    CHECK(NearlyEqual(EaseInOutSin(0.5f), 0.5f));
    // The in-out midpoints land on 0.5 for the odd-symmetric families.
    CHECK(NearlyEqual(EaseInOutQuartic(0.5f), 0.5f));
    CHECK(NearlyEqual(EaseInOutQuintic(0.5f), 0.5f));
    CHECK(NearlyEqual(EaseInOutCircular(0.5f), 0.5f));
}

// --- Math: Transform Lerp ---

TEST_CASE("math: Transform Lerp + identity")
{
    CHECK(NearlyEqual(IdentityTransform.position, Float3::Zero));
    CHECK(NearlyEqual(IdentityTransform.scale, Float3::One));

    Transform a{Float3{0, 0, 0}, Quaternion::Identity, Float3{1, 1, 1}};
    Transform b{Float3{2, 4, 6}, Quaternion::Identity, Float3{3, 3, 3}};
    Transform m = Transform::Lerp(a, b, 0.5f);
    CHECK(NearlyEqual(m.position, Float3{1, 2, 3}));
    CHECK(NearlyEqual(m.scale, Float3{2, 2, 2}));

    // Endpoints return the inputs.
    Transform at0 = Transform::Lerp(a, b, 0.0f);
    Transform at1 = Transform::Lerp(a, b, 1.0f);
    CHECK(NearlyEqual(at0.position, a.position));
    CHECK(NearlyEqual(at1.position, b.position));
}

// TRS compose then decompose round-trips; reparenting that preserves the world transform builds
// on it.
TEST_CASE("math: TRS decompose round-trips compose")
{
    Transform t;
    t.position = Float3{3.0f, -2.0f, 7.5f};
    t.rotation = Quaternion::FromAxisAngle(Normalized(Float3{0.3f, 1.0f, -0.2f}), 1.1f);
    t.scale = Float3{2.0f, 0.5f, 3.0f}; // non-uniform

    Float3 pos, scale;
    Quaternion rot;
    REQUIRE(Decompose(t.ToMatrix(), pos, rot, scale));

    CHECK(pos.x == doctest::Approx(t.position.x));
    CHECK(pos.y == doctest::Approx(t.position.y));
    CHECK(pos.z == doctest::Approx(t.position.z));
    CHECK(scale.x == doctest::Approx(t.scale.x).epsilon(0.001f));
    CHECK(scale.y == doctest::Approx(t.scale.y).epsilon(0.001f));
    CHECK(scale.z == doctest::Approx(t.scale.z).epsilon(0.001f));
    // Quaternions are sign-ambiguous: compare |dot| ~ 1.
    const f32 dot =
        t.rotation.x * rot.x + t.rotation.y * rot.y + t.rotation.z * rot.z + t.rotation.w * rot.w;
    CHECK(Abs(dot) == doctest::Approx(1.0f).epsilon(0.001f));

    // Transform::FromMatrix reproduces the same matrix.
    const Transform back = Transform::FromMatrix(t.ToMatrix());
    const Float4x4 m0 = t.ToMatrix();
    const Float4x4 m1 = back.ToMatrix();
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            CHECK(m1.m[r][c] == doctest::Approx(m0.m[r][c]).epsilon(0.001f));

    // Degenerate (zero scale) fails with identity outputs.
    Transform flat = t;
    flat.scale.y = 0.0f;
    CHECK_FALSE(Decompose(flat.ToMatrix(), pos, rot, scale));
    CHECK(scale.y == 1.0f);

    // Identity decomposes to identity.
    REQUIRE(Decompose(Float4x4::Identity(), pos, rot, scale));
    CHECK(pos.x == 0.0f);
    CHECK(scale.x == 1.0f);
    CHECK(rot.w == doctest::Approx(1.0f));
}

TEST_CASE("math: yaw/pitch/roll round-trips through quaternion")
{
    const f32 yaw = 0.8f, pitch = 0.4f, roll = -0.3f; // pitch within (-pi/2, pi/2)
    const Quaternion q = FromYawPitchRoll(yaw, pitch, roll);
    f32 y = 0, p = 0, r = 0;
    ToYawPitchRoll(q, y, p, r);
    CHECK(y == doctest::Approx(yaw).epsilon(0.001f));
    CHECK(p == doctest::Approx(pitch).epsilon(0.001f));
    CHECK(r == doctest::Approx(roll).epsilon(0.001f));

    // Axis sanity: pure yaw about Y matches FromAxisAngle.
    const Quaternion qy = FromYawPitchRoll(0.6f, 0.0f, 0.0f);
    const Quaternion qa = Quaternion::FromAxisAngle(Float3{0, 1, 0}, 0.6f);
    CHECK(Abs(qy.x * qa.x + qy.y * qa.y + qy.z * qa.z + qy.w * qa.w) ==
          doctest::Approx(1.0f).epsilon(0.001f));
}

TEST_CASE("math: RigidPart strips scale, keeps translation + rotation")
{
    // The nav-zone placement frame: a matrix must place world-unit-sized data without warping it.
    Transform t;
    t.position = Float3{3.0f, -2.0f, 7.5f};
    t.rotation = Quaternion::FromAxisAngle(Normalized(Float3{0.3f, 1.0f, 0.2f}), 0.9f);
    t.scale = Float3{2.0f, 0.5f, 4.0f};

    const Float4x4 rigid = RigidPart(t.ToMatrix());
    Float3 pos, scale;
    Quaternion rot;
    REQUIRE(Decompose(rigid, pos, rot, scale));
    CHECK(pos.x == doctest::Approx(3.0f));
    CHECK(pos.y == doctest::Approx(-2.0f));
    CHECK(pos.z == doctest::Approx(7.5f));
    CHECK(scale.x == doctest::Approx(1.0f));
    CHECK(scale.y == doctest::Approx(1.0f));
    CHECK(scale.z == doctest::Approx(1.0f));
    // Same rotation (allow the q/-q double cover).
    CHECK(Abs(rot.x * t.rotation.x + rot.y * t.rotation.y + rot.z * t.rotation.z +
              rot.w * t.rotation.w) == doctest::Approx(1.0f).epsilon(0.001f));

    // A unit-scale matrix passes through unchanged (within rounding).
    Transform u = t;
    u.scale = Float3::One;
    const Float4x4 same = RigidPart(u.ToMatrix());
    const Float4x4 orig = u.ToMatrix();
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            CHECK(same.m[r][c] == doctest::Approx(orig.m[r][c]).epsilon(0.001f));
        }
    }

    // A degenerate frame (a zero scale axis) cannot decompose; RigidPart then falls back to the
    // translation and the identity rotation, never garbage.
    Transform degenerate = t;
    degenerate.scale.y = 0.0f;
    const Float4x4 fallback = RigidPart(degenerate.ToMatrix());
    REQUIRE(Decompose(fallback, pos, rot, scale));
    CHECK(pos.x == doctest::Approx(3.0f));
    CHECK(rot.w == doctest::Approx(1.0f)); // identity rotation, not an arbitrary one
}
