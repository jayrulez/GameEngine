// Unit tests for the SIMD math types (:simd_vector, :simd_matrix).
//
// Strategy: the packed Float* types are the already-tested scalar reference. For each operation we
// compute it both ways (SIMD Vector*/Matrix4 vs packed Float*/Float4x4) over representative inputs and
// assert they agree. This validates whichever backend is compiled (SSE2 here on x86; the scalar
// fallback on other targets) against an independent implementation.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"
import draconic.foundation;

using namespace draconic::foundation;

namespace
{
    constexpr f32 kEps = 1.0e-4f;

    bool Eq(Vector2 v, Float2 f) { return NearlyEqual(v.ToFloat2(), f, kEps); }
    bool Eq(Vector3 v, Float3 f) { return NearlyEqual(v.ToFloat3(), f, kEps); }
    bool Eq(Vector4 v, Float4 f) { return NearlyEqual(v.ToFloat4(), f, kEps); }
    bool Eq(const Matrix4& m, const Float4x4& f) { return NearlyEqual(m.ToFloat4x4(), f, kEps); }
}

TEST_CASE("simd: types are 16-byte aligned")
{
    CHECK(alignof(Vector2) == 16);
    CHECK(alignof(Vector3) == 16);
    CHECK(alignof(Vector4) == 16);
    CHECK(alignof(Matrix4) == 16);
}

TEST_CASE("simd: Vector4 round-trips + arithmetic matches packed")
{
    const Float4 pa{1.0f, -2.0f, 3.5f, 4.0f};
    const Float4 pb{-0.5f, 2.0f, 1.0f, -3.0f};
    const Vector4 a(pa);
    const Vector4 b(pb);

    CHECK(Eq(a, pa)); // load/store round-trip
    CHECK(Eq(a + b, pa + pb));
    CHECK(Eq(a - b, pa - pb));
    CHECK(Eq(a * b, {pa.x * pb.x, pa.y * pb.y, pa.z * pb.z, pa.w * pb.w})); // component-wise
    CHECK(Eq(a * 2.5f, pa * 2.5f));
    CHECK(Eq(2.5f * a, 2.5f * pa));
    CHECK(Eq(a / 2.0f, pa * 0.5f));
    CHECK(Eq(-a, -pa));
    CHECK(Dot(a, b) == doctest::Approx(Dot(pa, pb)).epsilon(kEps));
    CHECK(Length(a) == doctest::Approx(Length(pa)).epsilon(kEps));
    CHECK(LengthSquared(a) == doctest::Approx(LengthSquared(pa)).epsilon(kEps));
    CHECK(Eq(Normalized(a), Normalized(pa)));
    CHECK(Eq(Lerp(a, b, 0.25f), Lerp(pa, pb, 0.25f)));
    CHECK(Eq(Min(a, b), {-0.5f, -2.0f, 1.0f, -3.0f}));
    CHECK(Eq(Max(a, b), {1.0f, 2.0f, 3.5f, 4.0f}));

    Vector4 c(pa);
    c += b;
    CHECK(Eq(c, pa + pb));
    c -= b;
    CHECK(Eq(c, pa));
    c *= 3.0f;
    CHECK(Eq(c, pa * 3.0f));
}

TEST_CASE("simd: Vector3 matches packed (incl. cross)")
{
    const Float3 pa{1.0f, 2.0f, 3.0f};
    const Float3 pb{-4.0f, 5.0f, -6.0f};
    const Vector3 a(pa);
    const Vector3 b(pb);

    CHECK(Eq(a, pa));
    CHECK(Eq(a + b, pa + pb));
    CHECK(Eq(a - b, pa - pb));
    CHECK(Eq(a * 2.0f, pa * 2.0f));
    CHECK(Eq(a / 4.0f, pa / 4.0f));
    CHECK(Eq(-a, -pa));
    CHECK(Dot(a, b) == doctest::Approx(Dot(pa, pb)).epsilon(kEps));
    CHECK(Eq(Cross(a, b), Cross(pa, pb)));
    CHECK(Length(a) == doctest::Approx(Length(pa)).epsilon(kEps));
    CHECK(Distance(a, b) == doctest::Approx(Distance(pa, pb)).epsilon(kEps));
    CHECK(Eq(Normalized(a), Normalized(pa)));
    CHECK(Eq(Lerp(a, b, 0.5f), Lerp(pa, pb, 0.5f)));
    CHECK(Eq(Min(a, b), Min(pa, pb)));
    CHECK(Eq(Max(a, b), Max(pa, pb)));
}

TEST_CASE("simd: Vector3 keeps w=0 invariant through ops")
{
    // Dot uses all 4 lanes; if any op leaked a nonzero w it would corrupt the result.
    const Vector3 a(2.0f, 3.0f, 4.0f);
    const Vector3 b(5.0f, 6.0f, 7.0f);
    const Vector3 c = (a + b) * 2.0f - Cross(a, b);
    CHECK(Dot(c, c) == doctest::Approx(LengthSquared(c.ToFloat3())).epsilon(kEps));
}

TEST_CASE("simd: Vector2 matches packed")
{
    const Float2 pa{3.0f, 4.0f};
    const Float2 pb{1.0f, -2.0f};
    const Vector2 a(pa);
    const Vector2 b(pb);

    CHECK(Eq(a, pa));
    CHECK(Eq(a + b, pa + pb));
    CHECK(Eq(a - b, pa - pb));
    CHECK(Eq(a * 2.0f, pa * 2.0f));
    CHECK(Dot(a, b) == doctest::Approx(Dot(pa, pb)).epsilon(kEps));
    CHECK(Length(a) == doctest::Approx(5.0f).epsilon(kEps)); // 3-4-5
    CHECK(Eq(Normalized(a), Normalized(pa)));
    CHECK(Eq(Lerp(a, b, 0.5f), Lerp(pa, pb, 0.5f)));
}

TEST_CASE("simd: near-zero Normalized returns zero")
{
    CHECK(Eq(Normalized(Vector3(0.0f, 0.0f, 0.0f)), Float3::Zero));
    CHECK(Eq(Normalized(Vector4(0.0f)), Float4::Zero));
}

TEST_CASE("simd: Matrix4 multiply / transform match packed")
{
    const Float4x4 t = Float4x4::Translation({3.0f, -1.0f, 2.0f});
    const Float4x4 r = Float4x4::RotationY(0.75f);
    const Float4x4 s = Float4x4::Scale({2.0f, 0.5f, 1.5f});
    const Float4x4 composed = s * r * t; // packed reference

    const Matrix4 st(s), rt(r), tt(t);
    const Matrix4 sim = st * rt * tt; // simd

    CHECK(Eq(Matrix4::Identity(), Float4x4::Identity()));
    CHECK(Eq(Matrix4(composed), composed)); // load/store round-trip
    CHECK(Eq(sim, composed));               // matmul equivalence
    CHECK(Eq(Transpose(sim), Transpose(composed)));

    const Float4 pv{1.5f, -2.0f, 0.5f, 1.0f};
    CHECK(Eq(Vector4(pv) * sim, pv * composed)); // row-vector transform

    const Float3 pp{4.0f, 5.0f, -6.0f};
    CHECK(Eq(TransformPoint(Vector3(pp), sim), TransformPoint(pp, composed)));
    CHECK(Eq(TransformDirection(Vector3(pp), sim), TransformDirection(pp, composed)));
}

TEST_CASE("simd: Matrix4 vs packed on a perspective/view chain")
{
    const Float4x4 view = Float4x4::LookAtRH({0, 3, 8}, {0, 0, 0}, {0, 1, 0});
    const Float4x4 proj = Float4x4::PerspectiveFovRH(1.0f, 16.0f / 9.0f, 0.1f, 100.0f);
    const Float4x4 vp = view * proj;

    const Matrix4 simVp = Matrix4(view) * Matrix4(proj);
    CHECK(Eq(simVp, vp));

    const Float3 worldPos{2.0f, 1.0f, -3.0f};
    const Float4 clipPacked = Float4(worldPos, 1.0f) * vp;
    const Float4 clipSimd = (Vector4(Float4(worldPos, 1.0f)) * simVp).ToFloat4();
    CHECK(NearlyEqual(clipSimd, clipPacked, 1.0e-3f)); // deeper accumulation, looser eps
}
