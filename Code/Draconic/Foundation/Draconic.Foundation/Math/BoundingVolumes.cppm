// Draconic Foundation - :bounds partition
//
// Bounding-volume suite ported from SedulousEngine (Sedulous.Core.Mathematics): ContainmentType,
// PlaneIntersectionType, Ray, BoundingSphere, BoundingFrustum, plus BoundingBox helpers over the
// existing AABB. The types are mutually recursive (ray↔sphere↔frustum↔box), so they live in ONE
// partition; cross-type Contains/Intersects are FREE FUNCTIONS defined after the structs (Draconic
// idiom; avoids C++ incomplete-type issues with Sedulous's member overloads). Sedulous's nullable
// `float?` ray results become `bool Intersects(..., f32& outT)` (false = no hit). Approximate
// comparisons use Sedulous's exact 1E-7 epsilon.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:bounds;

import :base;
import :math;
import :float3;
import :float4x4;
import :aabb;
import :plane;
import :span;

export namespace draconic::foundation
{
    enum class ContainmentType : u8
    {
        Disjoint,
        Contains,
        Intersects
    };
    enum class PlaneIntersectionType : u8
    {
        Front,
        Back,
        Intersecting
    };

    namespace bounds_detail
    {
        inline constexpr f32 kApprox = 1.0e-7f; // matches Sedulous MathUtil (float)
        [[nodiscard]] inline bool ApproxZero(f32 v) noexcept { return Abs(v) < kApprox; }
        [[nodiscard]] inline bool ApproxNonZero(f32 v) noexcept { return Abs(v) >= kApprox; }
        // Sedulous IsApproximatelyGreaterThan: equal counts as true; else needs a real gap AND >.
        [[nodiscard]] inline bool ApproxGreater(f32 a, f32 b) noexcept
        {
            return a == b || (Abs(a - b) >= kApprox && a > b);
        }
        [[nodiscard]] inline Float3 ClampVec(Float3 v, Float3 lo, Float3 hi) noexcept
        {
            return Max(Min(v, hi), lo);
        }
    }

    // =======================================================================
    // Ray - position + direction.
    // =======================================================================
    struct Ray
    {
        Float3 position;
        Float3 direction;

        [[nodiscard]] Ray Interpolate(const Ray& target, f32 t) const noexcept
        {
            return Ray{Lerp(position, target.position, t), Lerp(direction, target.direction, t)};
        }
    };

    // =======================================================================
    // BoundingSphere - center + radius.
    // =======================================================================
    struct BoundingSphere
    {
        Float3 center;
        f32 radius = 0.0f;

        [[nodiscard]] static BoundingSphere FromCenterRadius(Float3 c, f32 r) noexcept
        {
            return BoundingSphere{c, r};
        }

        // Merge two spheres into the smallest enclosing sphere (Sedulous CreateMerged).
        [[nodiscard]] static BoundingSphere Merge(const BoundingSphere& a,
                                                  const BoundingSphere& b) noexcept
        {
            const Float3 offset = b.center - a.center;
            const f32 distance = Length(offset);
            if (a.radius + b.radius >= distance)
            {
                if (distance <= a.radius - b.radius)
                {
                    return a;
                }
                if (distance <= b.radius - a.radius)
                {
                    return b;
                }
            }
            const Float3 n = offset * (1.0f / distance);
            const f32 mn = Min(-a.radius, distance - b.radius);
            const f32 mx = (Max(a.radius, distance + b.radius) - mn) * 0.5f;
            return BoundingSphere{a.center + n * (mx + mn), mx};
        }

        // Ritter-style enclosing sphere of a point set (Sedulous CreateFromPoints). NOTE: Sedulous's
        // X-axis branch has a copy-paste bug (Lerp(minX,minY)); ported CORRECTLY here (Lerp(minX,maxX)).
        [[nodiscard]] static BoundingSphere FromPoints(Span<const Float3> points) noexcept
        {
            if (points.IsEmpty())
            {
                return BoundingSphere{Float3{0, 0, 0}, 0.0f};
            }
            Float3 minX = points[0], maxX = points[0], minY = points[0], maxY = points[0],
                   minZ = points[0], maxZ = points[0];
            for (usize i = 1; i < points.Size(); ++i)
            {
                const Float3 p = points[i];
                if (p.x < minX.x)
                {
                    minX = p;
                }
                if (p.x > maxX.x)
                {
                    maxX = p;
                }
                if (p.y < minY.y)
                {
                    minY = p;
                }
                if (p.y > maxY.y)
                {
                    maxY = p;
                }
                if (p.z < minZ.z)
                {
                    minZ = p;
                }
                if (p.z > maxZ.z)
                {
                    maxZ = p;
                }
            }
            const f32 dX = Distance(minX, maxX), dY = Distance(minY, maxY),
                      dZ = Distance(minZ, maxZ);
            Float3 center;
            f32 radius;
            if (dX > dY && dX > dZ)
            {
                center = Lerp(minX, maxX, 0.5f);
                radius = dX * 0.5f;
            }
            else if (dY > dZ)
            {
                center = Lerp(minY, maxY, 0.5f);
                radius = dY * 0.5f;
            }
            else
            {
                center = Lerp(minZ, maxZ, 0.5f);
                radius = dZ * 0.5f;
            }
            for (usize i = 0; i < points.Size(); ++i)
            {
                const Float3 rel = points[i] - center;
                const f32 dist = Length(rel);
                if (dist > radius)
                {
                    radius = (radius + dist) * 0.5f;
                    center = center + rel * (1.0f - radius / dist);
                }
            }
            return BoundingSphere{center, radius};
        }

        void Expand(Float3 p) noexcept
        {
            const Float3 rel = p - center;
            const f32 dist = Length(rel);
            if (dist > radius)
            {
                const f32 nr = (radius + dist) * 0.5f;
                center = center + rel * (1.0f - nr / dist);
                radius = nr;
            }
        }

        [[nodiscard]] ContainmentType Contains(Float3 point) const noexcept
        {
            return LengthSquared(point - center) < radius * radius ? ContainmentType::Contains
                                                                   : ContainmentType::Disjoint;
        }

        // Signed-distance plane test (front = fully on the plane normal's positive side).
        [[nodiscard]] PlaneIntersectionType Intersects(const Plane& plane) const noexcept
        {
            const f32 dist = Dot(plane.normal, center) + plane.d;
            if (dist > radius)
            {
                return PlaneIntersectionType::Front;
            }
            if (dist < -radius)
            {
                return PlaneIntersectionType::Back;
            }
            return PlaneIntersectionType::Intersecting;
        }
    };

    // =======================================================================
    // BoundingFrustum - 6 planes + 8 corners derived from a view-projection matrix.
    // =======================================================================
    struct BoundingFrustum
    {
        static constexpr i32 kCornerCount = 8;
        static constexpr i32 kPlaneCount = 6;

        Float4x4 matrix{};
        Plane planes[kPlaneCount]{};
        Float3 corners[kCornerCount]{};

        BoundingFrustum() noexcept = default;
        explicit BoundingFrustum(const Float4x4& m) noexcept { SetMatrix(m); }

        [[nodiscard]] const Plane& Near() const noexcept { return planes[0]; }
        [[nodiscard]] const Plane& Far() const noexcept { return planes[1]; }
        [[nodiscard]] const Plane& Left() const noexcept { return planes[2]; }
        [[nodiscard]] const Plane& Right() const noexcept { return planes[3]; }
        [[nodiscard]] const Plane& Top() const noexcept { return planes[4]; }
        [[nodiscard]] const Plane& Bottom() const noexcept { return planes[5]; }

        // Gribb-Hartmann plane extraction for a row-vector row-major view-proj, NDC z in [0,1] - the exact
        // column combinations Sedulous uses (M{r}{c} 1-indexed -> m(r-1,c-1)); planes point OUTWARD.
        void SetMatrix(const Float4x4& m) noexcept
        {
            matrix = m;
            planes[0] = Plane{Float3{-m(0, 2), -m(1, 2), -m(2, 2)}, -m(3, 2)}; // Near
            planes[1] = Plane{Float3{m(0, 2) - m(0, 3), m(1, 2) - m(1, 3), m(2, 2) - m(2, 3)},
                              m(3, 2) - m(3, 3)}; // Far
            planes[2] = Plane{Float3{-m(0, 3) - m(0, 0), -m(1, 3) - m(1, 0), -m(2, 3) - m(2, 0)},
                              -m(3, 3) - m(3, 0)}; // Left
            planes[3] = Plane{Float3{m(0, 0) - m(0, 3), m(1, 0) - m(1, 3), m(2, 0) - m(2, 3)},
                              m(3, 0) - m(3, 3)}; // Right
            planes[4] = Plane{Float3{m(0, 1) - m(0, 3), m(1, 1) - m(1, 3), m(2, 1) - m(2, 3)},
                              m(3, 1) - m(3, 3)}; // Top
            planes[5] = Plane{Float3{-m(0, 3) - m(0, 1), -m(1, 3) - m(1, 1), -m(2, 3) - m(2, 1)},
                              -m(3, 3) - m(3, 1)}; // Bottom
            for (i32 i = 0; i < kPlaneCount; ++i)
            {
                NormalizePlane(planes[i]);
            }

            const Ray nl = PlaneRay(planes[0], planes[2]); // near ∩ left
            const Ray rn = PlaneRay(planes[3], planes[0]); // right ∩ near
            const Ray lf = PlaneRay(planes[2], planes[1]); // left ∩ far
            const Ray fr = PlaneRay(planes[1], planes[3]); // far ∩ right
            corners[0] = PlanePoint(planes[4], nl);
            corners[1] = PlanePoint(planes[4], rn);
            corners[2] = PlanePoint(planes[5], rn);
            corners[3] = PlanePoint(planes[5], nl);
            corners[4] = PlanePoint(planes[4], lf);
            corners[5] = PlanePoint(planes[4], fr);
            corners[6] = PlanePoint(planes[5], fr);
            corners[7] = PlanePoint(planes[5], lf);
        }

        [[nodiscard]] ContainmentType Contains(Float3 point) const noexcept
        {
            for (i32 i = 0; i < kPlaneCount; ++i)
            {
                if (bounds_detail::ApproxGreater(Dot(planes[i].normal, point) + planes[i].d, 0.0f))
                {
                    return ContainmentType::Disjoint;
                }
            }
            return ContainmentType::Contains;
        }

        [[nodiscard]] PlaneIntersectionType Intersects(const Plane& plane) const noexcept
        {
            bool front = false, back = false;
            for (i32 i = 0; i < kCornerCount; ++i)
            {
                if (Dot(corners[i], plane.normal) + plane.d > 0.0f)
                {
                    front = true;
                }
                else
                {
                    back = true;
                }
                if (front && back)
                {
                    return PlaneIntersectionType::Intersecting;
                }
            }
            return front ? PlaneIntersectionType::Front : PlaneIntersectionType::Back;
        }

    private:
        static void NormalizePlane(Plane& p) noexcept
        {
            const f32 len = Length(p.normal);
            p.normal /= len;
            p.d /= len;
        }
        [[nodiscard]] static Ray PlaneRay(const Plane& p1, const Plane& p2) noexcept
        {
            const Float3 dir = Cross(p1.normal, p2.normal);
            const Float3 a = p1.normal * p2.d - p2.normal * p1.d; // = -p1.d*p2.n + p2.d*p1.n
            const Float3 pos = Cross(a, dir) * (1.0f / LengthSquared(dir));
            return Ray{pos, dir};
        }
        [[nodiscard]] static Float3 PlanePoint(const Plane& p, const Ray& r) noexcept
        {
            const f32 dist = (-p.d - Dot(p.normal, r.position)) / Dot(p.normal, r.direction);
            return r.position + r.direction * dist;
        }
    };

    // ---- Frustum cross-type tests (free functions; frustum planes point outward) ----
    [[nodiscard]] inline ContainmentType Contains(const BoundingFrustum& f,
                                                  const BoundingSphere& s) noexcept
    {
        bool intersects = false;
        for (i32 i = 0; i < BoundingFrustum::kPlaneCount; ++i)
        {
            switch (s.Intersects(f.planes[i]))
            {
            case PlaneIntersectionType::Front:
                return ContainmentType::Disjoint;
            case PlaneIntersectionType::Intersecting:
                intersects = true;
                break;
            default:
                break;
            }
        }
        return intersects ? ContainmentType::Intersects : ContainmentType::Contains;
    }
    [[nodiscard]] inline bool Intersects(const BoundingFrustum& f, const BoundingSphere& s) noexcept
    {
        return Contains(f, s) != ContainmentType::Disjoint;
    }

    // ---- BoundingBox (AABB) helpers matching Sedulous BoundingBox ----
    [[nodiscard]] inline AABB BoundingBoxFromSphere(const BoundingSphere& s) noexcept
    {
        const Float3 c{s.radius, s.radius, s.radius};
        return AABB{s.center - c, s.center + c};
    }
    // Sedulous corner order (index 0 = (Min.x,Max.y,Max.z) ... 7 = (Min.x,Min.y,Min.z)).
    inline void GetCorners(const AABB& b, Float3 out[8]) noexcept
    {
        out[0] = Float3{b.min.x, b.max.y, b.max.z};
        out[1] = Float3{b.max.x, b.max.y, b.max.z};
        out[2] = Float3{b.max.x, b.min.y, b.max.z};
        out[3] = Float3{b.min.x, b.min.y, b.max.z};
        out[4] = Float3{b.min.x, b.max.y, b.min.z};
        out[5] = Float3{b.max.x, b.max.y, b.min.z};
        out[6] = Float3{b.max.x, b.min.y, b.min.z};
        out[7] = Float3{b.min.x, b.min.y, b.min.z};
    }
    [[nodiscard]] inline ContainmentType ContainsCT(const AABB& b, Float3 p) noexcept
    {
        return b.Contains(p) ? ContainmentType::Contains : ContainmentType::Disjoint;
    }
    [[nodiscard]] inline PlaneIntersectionType Intersects(const AABB& b,
                                                          const Plane& plane) noexcept
    {
        Float3 pos{plane.normal.x >= 0 ? b.min.x : b.max.x, plane.normal.y >= 0 ? b.min.y : b.max.y,
                   plane.normal.z >= 0 ? b.min.z : b.max.z};
        Float3 neg{plane.normal.x >= 0 ? b.max.x : b.min.x, plane.normal.y >= 0 ? b.max.y : b.min.y,
                   plane.normal.z >= 0 ? b.max.z : b.min.z};
        if (Dot(plane.normal, pos) + plane.d > 0.0f)
        {
            return PlaneIntersectionType::Front;
        }
        if (Dot(plane.normal, neg) + plane.d < 0.0f)
        {
            return PlaneIntersectionType::Back;
        }
        return PlaneIntersectionType::Intersecting;
    }
    [[nodiscard]] inline bool Intersects(const AABB& b, const BoundingSphere& s) noexcept
    {
        const Float3 clamped = bounds_detail::ClampVec(s.center, b.min, b.max);
        return LengthSquared(s.center - clamped) <= s.radius * s.radius;
    }
    [[nodiscard]] inline AABB TransformAABB(const AABB& b, const Float4x4& m) noexcept
    {
        const Float3 c = b.Center(), e = b.Extents();
        const Float3 nc{c.x * m(0, 0) + c.y * m(1, 0) + c.z * m(2, 0) + m(3, 0),
                        c.x * m(0, 1) + c.y * m(1, 1) + c.z * m(2, 1) + m(3, 1),
                        c.x * m(0, 2) + c.y * m(1, 2) + c.z * m(2, 2) + m(3, 2)};
        const Float3 ne{Abs(m(0, 0)) * e.x + Abs(m(1, 0)) * e.y + Abs(m(2, 0)) * e.z,
                        Abs(m(0, 1)) * e.x + Abs(m(1, 1)) * e.y + Abs(m(2, 1)) * e.z,
                        Abs(m(0, 2)) * e.x + Abs(m(1, 2)) * e.y + Abs(m(2, 2)) * e.z};
        return AABB{nc - ne, nc + ne};
    }

    // Frustum ∩ box (needs the AABB↔Plane test above).
    [[nodiscard]] inline ContainmentType Contains(const BoundingFrustum& f, const AABB& b) noexcept
    {
        bool intersects = false;
        for (i32 i = 0; i < BoundingFrustum::kPlaneCount; ++i)
        {
            switch (Intersects(b, f.planes[i]))
            {
            case PlaneIntersectionType::Front:
                return ContainmentType::Disjoint;
            case PlaneIntersectionType::Intersecting:
                intersects = true;
                break;
            default:
                break;
            }
        }
        return intersects ? ContainmentType::Intersects : ContainmentType::Contains;
    }
    [[nodiscard]] inline bool Intersects(const BoundingFrustum& f, const AABB& b) noexcept
    {
        return Contains(f, b) != ContainmentType::Disjoint;
    }

    // AABB Contains sphere/frustum (ContainmentType), matching Sedulous BoundingBox.
    [[nodiscard]] inline ContainmentType ContainsCT(const AABB& b, const BoundingSphere& s) noexcept
    {
        const Float3 clamped = bounds_detail::ClampVec(s.center, b.min, b.max);
        if (s.radius * s.radius <= LengthSquared(s.center - clamped))
        {
            return ContainmentType::Disjoint;
        }
        if (s.center.x > b.max.x - s.radius || s.center.y > b.max.y - s.radius ||
            s.center.z > b.max.z - s.radius || b.min.x + s.radius > s.center.x ||
            b.min.y + s.radius > s.center.y || b.min.z + s.radius > s.center.z ||
            b.max.x - b.min.x <= s.radius || b.max.y - b.min.y <= s.radius ||
            b.max.z - b.min.z <= s.radius)
        {
            return ContainmentType::Intersects;
        }
        return ContainmentType::Contains;
    }

    // BoundingSphere cross-type (free functions).
    [[nodiscard]] inline bool Intersects(const BoundingSphere& a, const BoundingSphere& b) noexcept
    {
        const f32 combined = a.radius + b.radius;
        return LengthSquared(a.center - b.center) <= combined * combined;
    }
    [[nodiscard]] inline bool Intersects(const BoundingSphere& s, const AABB& b) noexcept
    {
        return Intersects(b, s);
    }
    [[nodiscard]] inline bool Intersects(const BoundingSphere& s, const BoundingFrustum& f) noexcept
    {
        return Intersects(f, s);
    }
    [[nodiscard]] inline ContainmentType Contains(const BoundingSphere& s,
                                                  const BoundingFrustum& f) noexcept
    {
        if (!Intersects(f, s))
        {
            return ContainmentType::Disjoint;
        }
        for (i32 i = 0; i < BoundingFrustum::kCornerCount; ++i)
        {
            if (s.Contains(f.corners[i]) == ContainmentType::Disjoint)
            {
                return ContainmentType::Intersects;
            }
        }
        return ContainmentType::Contains;
    }
    [[nodiscard]] inline BoundingSphere BoundingSphereFromFrustum(const BoundingFrustum& f) noexcept
    {
        return BoundingSphere::FromPoints(
            Span<const Float3>{f.corners, BoundingFrustum::kCornerCount});
    }

    // Ray intersections (bool + out distance; false = no hit). Faithful to Sedulous's float? results.
    [[nodiscard]] inline bool Intersects(const Ray& ray, const Plane& plane, f32& outT) noexcept
    {
        const f32 nDotDir = Dot(plane.normal, ray.direction);
        if (bounds_detail::ApproxZero(nDotDir))
        {
            return false;
        }
        const f32 dist = -(Dot(plane.normal, ray.position) + plane.d) / nDotDir;
        if (bounds_detail::ApproxZero(dist))
        {
            outT = 0.0f;
            return true;
        }
        if (dist < 0.0f)
        {
            return false;
        }
        outT = dist;
        return true;
    }
    [[nodiscard]] inline bool Intersects(const Ray& ray, const BoundingSphere& sphere,
                                         f32& outT) noexcept
    {
        const f32 r2 = sphere.radius * sphere.radius;
        const Float3 offset = sphere.center - ray.position;
        const f32 offLen2 = LengthSquared(offset);
        if (offLen2 < r2)
        {
            outT = 0.0f;
            return true;
        }
        const f32 toCenter = Dot(ray.direction, offset);
        if (toCenter < 0.0f)
        {
            return false;
        }
        const f32 toSphere = r2 + toCenter * toCenter - offLen2;
        if (toSphere < 0.0f)
        {
            return false;
        }
        outT = toCenter - Sqrt(toSphere);
        return true;
    }
    [[nodiscard]] inline bool Intersects(const Ray& ray, const AABB& box, f32& outT) noexcept
    {
        bool hasMin = false, hasMax = false;
        f32 mn = 0.0f, mx = 0.0f;
        // X
        if (bounds_detail::ApproxZero(ray.direction.x))
        {
            if (ray.position.x < box.min.x || ray.position.x > box.max.x)
            {
                return false;
            }
        }
        else
        {
            mn = (box.min.x - ray.position.x) / ray.direction.x;
            mx = (box.max.x - ray.position.x) / ray.direction.x;
            if (mn > mx)
            {
                const f32 t = mn;
                mn = mx;
                mx = t;
            }
            hasMin = hasMax = true;
        }
        // Y
        if (bounds_detail::ApproxZero(ray.direction.y))
        {
            if (ray.position.y < box.min.y || ray.position.y > box.max.y)
            {
                return false;
            }
        }
        else
        {
            f32 y0 = (box.min.y - ray.position.y) / ray.direction.y,
                y1 = (box.max.y - ray.position.y) / ray.direction.y;
            if (y0 > y1)
            {
                const f32 t = y0;
                y0 = y1;
                y1 = t;
            }
            if (!hasMin || y0 > mn)
            {
                mn = y0;
                hasMin = true;
            }
            if (!hasMax || y1 > mx)
            {
                mx = y1;
                hasMax = true;
            }
        }
        // Z
        if (bounds_detail::ApproxZero(ray.direction.z))
        {
            if (ray.position.z < box.min.z || ray.position.z > box.max.z)
            {
                return false;
            }
        }
        else
        {
            f32 z0 = (box.min.z - ray.position.z) / ray.direction.z,
                z1 = (box.max.z - ray.position.z) / ray.direction.z;
            if (z0 > z1)
            {
                const f32 t = z0;
                z0 = z1;
                z1 = t;
            }
            if (!hasMin || z0 > mn)
            {
                mn = z0;
                hasMin = true;
            }
            if (!hasMax || z1 > mx)
            {
                mx = z1;
                hasMax = true;
            }
        }
        if (hasMin && mn < 0.0f && mx > 0.0f)
        {
            outT = 0.0f;
            return true;
        }
        if (mn < 0.0f)
        {
            return false;
        }
        outT = mn;
        return true;
    }
    [[nodiscard]] inline bool Intersects(const BoundingFrustum& f, const Ray& ray,
                                         f32& outT) noexcept
    {
        if (f.Contains(ray.position) == ContainmentType::Contains)
        {
            outT = 0.0f;
            return true;
        }
        f32 mx = -kFloatMax, mn = kFloatMax;
        for (i32 i = 0; i < BoundingFrustum::kPlaneCount; ++i)
        {
            const Float3 n = f.planes[i].normal;
            const f32 dirDotN = Dot(ray.direction, n);
            const f32 posDotN = Dot(ray.position, n) + f.planes[i].d;
            if (bounds_detail::ApproxNonZero(dirDotN))
            {
                const f32 value = -posDotN / dirDotN;
                if (dirDotN < 0.0f)
                {
                    if (value > mn)
                    {
                        return false;
                    }
                    if (value > mx)
                    {
                        mx = value;
                    }
                }
                else
                {
                    if (value < mx)
                    {
                        return false;
                    }
                    if (value < mn)
                    {
                        mn = value;
                    }
                }
            }
            else if (posDotN > 0.0f)
            {
                return false;
            }
        }
        const f32 dist = mx >= 0.0f ? mx : mn;
        if (dist < 0.0f)
        {
            return false;
        }
        outT = dist;
        return true;
    }
    [[nodiscard]] inline bool Intersects(const Ray& ray, const BoundingFrustum& f,
                                         f32& outT) noexcept
    {
        return Intersects(f, ray, outT);
    }

    // ---- remaining Contains/Intersects overloads (complete the suite vs Sedulous) ----
    // sphere contains sphere
    [[nodiscard]] inline ContainmentType Contains(const BoundingSphere& a,
                                                  const BoundingSphere& b) noexcept
    {
        const f32 d2 = LengthSquared(a.center - b.center);
        const f32 combined = a.radius + b.radius;
        if (d2 > combined * combined)
        {
            return ContainmentType::Disjoint;
        }
        const f32 sub = a.radius - b.radius;
        return (sub * sub < d2) ? ContainmentType::Intersects : ContainmentType::Contains;
    }
    // sphere contains box (corner test, then closest-point distance)
    [[nodiscard]] inline ContainmentType Contains(const BoundingSphere& s, const AABB& box) noexcept
    {
        Float3 c[8];
        GetCorners(box, c);
        bool inside = true;
        for (i32 i = 0; i < 8; ++i)
        {
            if (s.Contains(c[i]) == ContainmentType::Disjoint)
            {
                inside = false;
                break;
            }
        }
        if (inside)
        {
            return ContainmentType::Contains;
        }
        f32 dist = 0.0f;
        if (s.center.x < box.min.x)
        {
            dist += (s.center.x - box.min.x) * (s.center.x - box.min.x);
        }
        else if (s.center.x > box.max.x)
        {
            dist += (s.center.x - box.max.x) * (s.center.x - box.max.x);
        }
        if (s.center.y < box.min.y)
        {
            dist += (s.center.y - box.min.y) * (s.center.y - box.min.y);
        }
        else if (s.center.y > box.max.y)
        {
            dist += (s.center.y - box.max.y) * (s.center.y - box.max.y);
        }
        if (s.center.z < box.min.z)
        {
            dist += (s.center.z - box.min.z) * (s.center.z - box.min.z);
        }
        else if (s.center.z > box.max.z)
        {
            dist += (s.center.z - box.max.z) * (s.center.z - box.max.z);
        }
        return (dist <= s.radius * s.radius) ? ContainmentType::Intersects
                                             : ContainmentType::Disjoint;
    }
    // box contains box / box contains frustum
    [[nodiscard]] inline ContainmentType ContainsCT(const AABB& b, const AABB& o) noexcept
    {
        if (o.max.x < b.min.x || o.min.x > b.max.x || o.max.y < b.min.y || o.min.y > b.max.y ||
            o.max.z < b.min.z || o.min.z > b.max.z)
        {
            return ContainmentType::Disjoint;
        }
        if (o.min.x >= b.min.x && o.max.x <= b.max.x && o.min.y >= b.min.y && o.max.y <= b.max.y &&
            o.min.z >= b.min.z && o.max.z <= b.max.z)
        {
            return ContainmentType::Contains;
        }
        return ContainmentType::Intersects;
    }
    [[nodiscard]] inline ContainmentType ContainsCT(const AABB& b,
                                                    const BoundingFrustum& f) noexcept
    {
        if (!Intersects(f, b))
        {
            return ContainmentType::Disjoint;
        }
        for (i32 i = 0; i < BoundingFrustum::kCornerCount; ++i)
        {
            if (ContainsCT(b, f.corners[i]) == ContainmentType::Disjoint)
            {
                return ContainmentType::Intersects;
            }
        }
        return ContainmentType::Contains;
    }
    [[nodiscard]] inline bool Intersects(const AABB& b, const BoundingFrustum& f) noexcept
    {
        return Intersects(f, b);
    }
    // frustum contains / intersects frustum
    [[nodiscard]] inline ContainmentType Contains(const BoundingFrustum& f,
                                                  const BoundingFrustum& g) noexcept
    {
        bool intersection = false;
        for (i32 i = 0; i < BoundingFrustum::kPlaneCount; ++i)
        {
            switch (g.Intersects(f.planes[i]))
            {
            case PlaneIntersectionType::Front:
                return ContainmentType::Disjoint;
            case PlaneIntersectionType::Intersecting:
                intersection = true;
                break;
            default:
                break;
            }
        }
        return intersection ? ContainmentType::Intersects : ContainmentType::Contains;
    }
    [[nodiscard]] inline bool Intersects(const BoundingFrustum& f,
                                         const BoundingFrustum& g) noexcept
    {
        return Contains(f, g) != ContainmentType::Disjoint;
    }
}
