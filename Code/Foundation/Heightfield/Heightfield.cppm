// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Heightfield - `foundation.heightfield`.
///
/// The pure heightfield grid and its sampling math: the shared source of truth the terrain
/// renderer, the physics collider and the navigation bake all read, so none of them depends on the
/// terrain renderer for heights. No RHI and no terrain concepts; depends only on Foundation::Core.
///
/// A grid is square with side S = 64*k + 1 (k >= 1: 65, 129, 257, ...). That one rule tiles into
/// 64-quad (65-vertex) render chunks sharing edges and satisfies Jolt's square HeightFieldShape. u16
/// samples map linearly onto a world Y range [minY, maxY] over a worldSize (X by Z) footprint centered
/// on the local origin (XZ plane, +Y up); the entity transform places it in the world.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.heightfield;

import foundation.core;

using namespace foundation::core;

export namespace foundation::heightfield
{
    /// One quantized height sample (0 = minY, 65535 = maxY).
    using Height = u16;

    /// True when `size` is a legal heightfield side: square grids of S = 64*k + 1, k >= 1.
    [[nodiscard]] constexpr bool IsValidSize(i32 size) noexcept
    {
        return size >= 65 && ((size - 1) % 64) == 0;
    }

    /// The next legal size >= `size` (a convenience for import resampling; never below 65).
    [[nodiscard]] constexpr i32 NextValidSize(i32 size) noexcept
    {
        if (size <= 65)
        {
            return 65;
        }
        const i32 chunks = (size - 1 + 63) / 64; // ceil((size-1)/64)
        return chunks * 64 + 1;
    }

    /// A square heightfield: S*S u16 samples (row-major, index = gx + gz*S) over a world footprint.
    ///
    /// It is an Object so it can be a referenceable resource product (Ref<Heightfield>), as
    /// StaticMesh is; terrain, the physics collider and the nav bake all hold a Ref to one. The grid
    /// and sampling math still depend only on Foundation::Core; the cooked source and factory live
    /// in foundation.heightfield.resource.
    class Heightfield : public Object
    {
        RTTI_OBJECT(Heightfield, Object)
    public:
        // Unique per-object id. GPU caches (the terrain height-texture cache) key by this, never
        // by pointer: a freed heightfield's address can be reused by a fresh grid at the same
        // version (fresh grids all start at 1), and pointer keying would then serve the dead
        // grid's texture. StaticMesh::uid does the same.
        const u64 uid = NextUid();

        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        Heightfield() = default;

        /// Allocate a zeroed grid. `size` must satisfy IsValidSize; worldSize is the XZ footprint,
        /// [minY, maxY] the world Y range the u16 samples span (maxY > minY).
        Heightfield(i32 size, Float2 worldSize, f32 minY, f32 maxY)
            : m_size(size), m_worldSize(worldSize), m_minY(minY), m_maxY(maxY)
        {
            m_samples.Resize(static_cast<usize>(size) * static_cast<usize>(size), Height{0});
            m_holes.Resize(static_cast<usize>(size) * static_cast<usize>(size), u8{0});
        }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_size <= 0; }
        [[nodiscard]] i32 Size() const noexcept { return m_size; }
        [[nodiscard]] Float2 WorldSize() const noexcept { return m_worldSize; }
        [[nodiscard]] f32 MinY() const noexcept { return m_minY; }
        [[nodiscard]] f32 MaxY() const noexcept { return m_maxY; }

        /// Monotonic edit generation (starts at 1). Bump it after rewriting samples so downstream
        /// GPU caches (the terrain height texture) re-upload, as sculpting and regeneration do.
        [[nodiscard]] u64 Version() const noexcept { return m_version; }
        void BumpVersion() noexcept { ++m_version; }

        // ---- raw sample access (grid indices clamped to [0, S-1]) ----
        [[nodiscard]] Height GetSample(i32 gx, i32 gz) const noexcept
        {
            return m_samples[Index(gx, gz)];
        }
        void SetSample(i32 gx, i32 gz, Height h) noexcept { m_samples[Index(gx, gz)] = h; }

        [[nodiscard]] Span<const Height> Samples() const noexcept
        {
            return Span<const Height>{m_samples.Data(), m_samples.Size()};
        }
        [[nodiscard]] Span<Height> Samples() noexcept
        {
            return Span<Height>{m_samples.Data(), m_samples.Size()};
        }

        // ---- holes: a per-sample cut -------------------------------------------------------
        // The one rule every consumer applies: a triangle with a hole vertex is removed. The
        // renderer's chunk indices, Jolt's no-collision sample, the nav bake's blocks, the
        // vegetation's placement and QueryRay all read the same plane. A byte per sample,
        // 0 = solid, 255 = cut, laid out like the heights; the plane is the "holes" stream of the
        // cooked form. The count keeps every consumer's "no holes here" O(1).
        [[nodiscard]] bool IsHole(i32 gx, i32 gz) const noexcept
        {
            return m_holes[Index(gx, gz)] != 0;
        }
        void SetHole(i32 gx, i32 gz, bool hole) noexcept
        {
            u8& h = m_holes[Index(gx, gz)];
            if ((h != 0) == hole)
            {
                return;
            }
            h = hole ? u8{255} : u8{0};
            if (hole)
            {
                ++m_holeCount;
            }
            else
            {
                --m_holeCount;
            }
        }
        [[nodiscard]] Span<const u8> Holes() const noexcept
        {
            return Span<const u8>{m_holes.Data(), m_holes.Size()};
        }
        /// Replace the whole plane (the cooked "holes" stream); a blob of any other size is
        /// refused (false) and the plane is left as it was.
        bool SetHoles(Span<const u8> plane) noexcept
        {
            if (plane.Size() != m_holes.Size())
            {
                return false;
            }
            m_holeCount = 0;
            for (usize i = 0; i < plane.Size(); ++i)
            {
                m_holes[i] = plane[i] != 0 ? u8{255} : u8{0};
                m_holeCount += plane[i] != 0 ? 1u : 0u;
            }
            return true;
        }
        [[nodiscard]] u32 HoleCount() const noexcept { return m_holeCount; }
        [[nodiscard]] bool HasHoles() const noexcept { return m_holeCount != 0; }
        /// The cell (cx, cz), the quad between samples cx..cx+1, cz..cz+1, has a cut corner:
        /// both of its triangles are gone. Indices clamp like every other accessor.
        [[nodiscard]] bool CellHasHole(i32 cx, i32 cz) const noexcept
        {
            if (m_holeCount == 0)
            {
                return false;
            }
            return IsHole(cx, cz) || IsHole(cx + 1, cz) || IsHole(cx, cz + 1) || IsHole(cx + 1, cz + 1);
        }
        /// Any cut sample in the inclusive block [gx0, gx1] x [gz0, gz1]: the coarse-LOD quad and
        /// the nav bake's stride block ask this (a hole never shrinks with distance).
        [[nodiscard]] bool BlockHasHole(i32 gx0, i32 gz0, i32 gx1, i32 gz1) const noexcept
        {
            if (m_holeCount == 0)
            {
                return false;
            }
            const i32 x0 = Clamp(gx0, 0, m_size - 1);
            const i32 x1 = Clamp(gx1, 0, m_size - 1);
            const i32 z0 = Clamp(gz0, 0, m_size - 1);
            const i32 z1 = Clamp(gz1, 0, m_size - 1);
            for (i32 gz = z0; gz <= z1; ++gz)
            {
                for (i32 gx = x0; gx <= x1; ++gx)
                {
                    if (m_holes[Index(gx, gz)] != 0)
                    {
                        return true;
                    }
                }
            }
            return false;
        }
        /// The cell a local XZ position lies in (clamped to the grid's cells).
        void CellOfLocal(f32 localX, f32 localZ, i32& outCx, i32& outCz) const noexcept
        {
            const Float2 g = WorldToGrid(localX, localZ);
            outCx = Clamp(static_cast<i32>(Floor(g.x)), 0, m_size - 2);
            outCz = Clamp(static_cast<i32>(Floor(g.y)), 0, m_size - 2);
        }

        // ---- quantization (sample <-> world Y) ----
        [[nodiscard]] f32 SampleToWorldY(f32 sample) const noexcept
        {
            return m_minY + (sample / 65535.0f) * (m_maxY - m_minY);
        }
        [[nodiscard]] Height WorldYToSample(f32 worldY) const noexcept
        {
            const f32 range = m_maxY - m_minY;
            const f32 t = (range > 0.0f) ? (worldY - m_minY) / range : 0.0f;
            const f32 clamped = Clamp01(t) * 65535.0f;
            return static_cast<Height>(Round(clamped));
        }

        // ---- world XZ <-> grid coordinate (grid space is [0, S-1], footprint centered on origin) ----
        [[nodiscard]] Float2 WorldToGrid(f32 worldX, f32 worldZ) const noexcept
        {
            const f32 span = static_cast<f32>(m_size - 1);
            const f32 gx = (worldX / m_worldSize.x + 0.5f) * span;
            const f32 gz = (worldZ / m_worldSize.y + 0.5f) * span;
            return Float2{gx, gz};
        }
        [[nodiscard]] Float2 GridToWorld(f32 gridX, f32 gridZ) const noexcept
        {
            const f32 span = static_cast<f32>(m_size - 1);
            const f32 wx = (gridX / span - 0.5f) * m_worldSize.x;
            const f32 wz = (gridZ / span - 0.5f) * m_worldSize.y;
            return Float2{wx, wz};
        }

        // ---- height queries (world Y) ----
        /// Nearest grid point height (exact at grid samples).
        [[nodiscard]] f32 GetHeightAtGrid(i32 gx, i32 gz) const noexcept
        {
            return SampleToWorldY(static_cast<f32>(GetSample(gx, gz)));
        }

        /// Bilinear height at a world XZ point; positions outside the footprint clamp to the edge.
        [[nodiscard]] f32 GetHeightAt(f32 worldX, f32 worldZ) const noexcept
        {
            if (IsEmpty())
            {
                return 0.0f;
            }
            const Float2 g = WorldToGrid(worldX, worldZ);
            const f32 span = static_cast<f32>(m_size - 1);
            const f32 cgx = Clamp(g.x, 0.0f, span);
            const f32 cgz = Clamp(g.y, 0.0f, span);
            const i32 x0 = Clamp(static_cast<i32>(Floor(cgx)), 0, m_size - 2 < 0 ? 0 : m_size - 2);
            const i32 z0 = Clamp(static_cast<i32>(Floor(cgz)), 0, m_size - 2 < 0 ? 0 : m_size - 2);
            const f32 fx = cgx - static_cast<f32>(x0);
            const f32 fz = cgz - static_cast<f32>(z0);
            const f32 h00 = static_cast<f32>(GetSample(x0, z0));
            const f32 h10 = static_cast<f32>(GetSample(x0 + 1, z0));
            const f32 h01 = static_cast<f32>(GetSample(x0, z0 + 1));
            const f32 h11 = static_cast<f32>(GetSample(x0 + 1, z0 + 1));
            const f32 top = h00 + (h10 - h00) * fx;
            const f32 bottom = h01 + (h11 - h01) * fx;
            return SampleToWorldY(top + (bottom - top) * fz);
        }

        /// Surface normal (world space, unit) via central differences one cell wide.
        [[nodiscard]] Float3 GetNormalAt(f32 worldX, f32 worldZ) const noexcept
        {
            if (IsEmpty())
            {
                return Float3{0.0f, 1.0f, 0.0f};
            }
            const f32 ex = m_worldSize.x / static_cast<f32>(m_size - 1);
            const f32 ez = m_worldSize.y / static_cast<f32>(m_size - 1);
            const f32 dhdx = (GetHeightAt(worldX + ex, worldZ) - GetHeightAt(worldX - ex, worldZ)) /
                             (2.0f * ex);
            const f32 dhdz = (GetHeightAt(worldX, worldZ + ez) - GetHeightAt(worldX, worldZ - ez)) /
                             (2.0f * ez);
            return Normalized(Float3{-dhdx, 1.0f, -dhdz});
        }

        /// Min/max world Y over the inclusive grid block [gx0..gx1] x [gz0..gz1] (chunk bounds).
        void CellBounds(i32 gx0, i32 gz0, i32 gx1, i32 gz1, f32& outMinY, f32& outMaxY) const noexcept
        {
            Height lo = 65535;
            Height hi = 0;
            for (i32 z = gz0; z <= gz1; ++z)
            {
                for (i32 x = gx0; x <= gx1; ++x)
                {
                    const Height s = GetSample(x, z);
                    lo = s < lo ? s : lo;
                    hi = s > hi ? s : hi;
                }
            }
            outMinY = SampleToWorldY(static_cast<f32>(lo));
            outMaxY = SampleToWorldY(static_cast<f32>(hi));
        }

        /// Local-space ray against the surface. Returns the nearest hit distance in `outT` (>= 0).
        /// A height-field ray march: clip to the grid AABB, step by ~half a cell, and bisect the
        /// crossing where the ray drops to/through the bilinear surface. Robust for editor picking.
        /// A crossing inside a cut cell is no surface: the ray passes through to whatever sits
        /// below, as the gameplay trace does.
        [[nodiscard]] bool QueryRay(Float3 origin, Float3 direction, f32& outT) const noexcept
        {
            return MarchRay(origin, direction, outT, /*skipHoles*/ true);
        }

        /// The same march with the cut samples treated as surface: the brushes that work on the
        /// hole (Fill, the prop brush erasing what stands over a cut) pick the plane where the
        /// terrain would be instead of falling through it. Never a gameplay query.
        [[nodiscard]] bool QueryRayIgnoringHoles(Float3 origin, Float3 direction,
                                                 f32& outT) const noexcept
        {
            return MarchRay(origin, direction, outT, /*skipHoles*/ false);
        }

    private:
        [[nodiscard]] bool MarchRay(Float3 origin, Float3 direction, f32& outT,
                                    bool skipHoles) const noexcept
        {
            if (IsEmpty())
            {
                return false;
            }
            const Float3 dir = Normalized(direction);
            const f32 halfX = m_worldSize.x * 0.5f;
            const f32 halfZ = m_worldSize.y * 0.5f;
            f32 t0 = 0.0f;
            f32 t1 = 0.0f;
            if (!ClipToAabb(origin, dir, Float3{-halfX, m_minY, -halfZ}, Float3{halfX, m_maxY, halfZ},
                            t0, t1))
            {
                return false;
            }

            // Step so horizontal travel is ~half a cell; for near-vertical rays fall back to Y range.
            const f32 cell = m_worldSize.x / static_cast<f32>(m_size - 1);
            const f32 horizSpeed = Sqrt(dir.x * dir.x + dir.z * dir.z);
            const f32 vertSpeed = Abs(dir.y);
            const f32 speed = horizSpeed > vertSpeed ? horizSpeed : vertSpeed;
            const f32 step = (speed > 1.0e-6f) ? (0.5f * cell / speed) : (0.5f * cell);

            f32 tPrev = t0;
            f32 diffPrev = SignedGap(origin, dir, t0);
            if (diffPrev <= 0.0f && !(skipHoles && HoleAt(origin, dir, t0))) // at/under the surface at entry
            {
                outT = t0;
                return true;
            }
            for (f32 t = t0 + step; t <= t1 + step; t += step)
            {
                const f32 tc = t < t1 ? t : t1;
                const f32 diff = SignedGap(origin, dir, tc);
                if (diffPrev > 0.0f && diff <= 0.0f) // crossed the surface between tPrev and tc
                {
                    f32 lo = tPrev;
                    f32 hi = tc;
                    for (i32 i = 0; i < 12; ++i) // bisect to refine
                    {
                        const f32 mid = 0.5f * (lo + hi);
                        if (SignedGap(origin, dir, mid) > 0.0f)
                        {
                            lo = mid;
                        }
                        else
                        {
                            hi = mid;
                        }
                    }
                    const f32 hit = 0.5f * (lo + hi);
                    // A crossing inside a cut cell is no surface: the ray passes through to
                    // whatever sits below (a cave floor, the physics world) and the march goes
                    // on, needing to come back above the field before another crossing counts.
                    if (!(skipHoles && HoleAt(origin, dir, hit)))
                    {
                        outT = hit;
                        return true;
                    }
                }
                tPrev = tc;
                diffPrev = diff;
                if (tc >= t1)
                {
                    break;
                }
            }
            return false;
        }

        [[nodiscard]] usize Index(i32 gx, i32 gz) const noexcept
        {
            const i32 cx = Clamp(gx, 0, m_size - 1);
            const i32 cz = Clamp(gz, 0, m_size - 1);
            return static_cast<usize>(cx) + static_cast<usize>(cz) * static_cast<usize>(m_size);
        }

        // The cell under the ray point at distance t has a cut corner (no surface there).
        [[nodiscard]] bool HoleAt(Float3 origin, Float3 dir, f32 t) const noexcept
        {
            if (m_holeCount == 0)
            {
                return false;
            }
            i32 cx = 0;
            i32 cz = 0;
            CellOfLocal(origin.x + dir.x * t, origin.z + dir.z * t, cx, cz);
            return CellHasHole(cx, cz);
        }

        // Ray height above the surface at distance t (positive above, negative below).
        [[nodiscard]] f32 SignedGap(Float3 origin, Float3 dir, f32 t) const noexcept
        {
            const f32 px = origin.x + dir.x * t;
            const f32 py = origin.y + dir.y * t;
            const f32 pz = origin.z + dir.z * t;
            return py - GetHeightAt(px, pz);
        }

        // Slab clip of a ray against an AABB; returns the [t0, t1] overlap (t0 >= 0), false if none.
        [[nodiscard]] static bool ClipToAabb(Float3 o, Float3 d, Float3 lo, Float3 hi, f32& t0,
                                             f32& t1) noexcept
        {
            f32 tmin = 0.0f;
            f32 tmax = 3.402823466e38f;
            const f32 oa[3] = {o.x, o.y, o.z};
            const f32 da[3] = {d.x, d.y, d.z};
            const f32 la[3] = {lo.x, lo.y, lo.z};
            const f32 ha[3] = {hi.x, hi.y, hi.z};
            for (i32 a = 0; a < 3; ++a)
            {
                if (Abs(da[a]) < 1.0e-8f)
                {
                    if (oa[a] < la[a] || oa[a] > ha[a])
                    {
                        return false; // parallel and outside the slab
                    }
                }
                else
                {
                    const f32 inv = 1.0f / da[a];
                    f32 tNear = (la[a] - oa[a]) * inv;
                    f32 tFar = (ha[a] - oa[a]) * inv;
                    if (tNear > tFar)
                    {
                        const f32 tmp = tNear;
                        tNear = tFar;
                        tFar = tmp;
                    }
                    tmin = tNear > tmin ? tNear : tmin;
                    tmax = tFar < tmax ? tFar : tmax;
                    if (tmin > tmax)
                    {
                        return false;
                    }
                }
            }
            t0 = tmin;
            t1 = tmax;
            return true;
        }

        [[nodiscard]] static f32 Clamp(f32 v, f32 lo, f32 hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }
        [[nodiscard]] static i32 Clamp(i32 v, i32 lo, i32 hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }
        [[nodiscard]] static f32 Clamp01(f32 v) noexcept { return Clamp(v, 0.0f, 1.0f); }

        i32 m_size = 0;
        Float2 m_worldSize{0.0f, 0.0f};
        f32 m_minY = 0.0f;
        f32 m_maxY = 0.0f;
        u64 m_version = 1; // edit generation (BumpVersion) for GPU-cache invalidation
        Array<Height> m_samples;
        Array<u8> m_holes;   // the per-sample cut plane (0 solid, 255 cut), same layout as the heights
        u32 m_holeCount = 0; // cut samples: every consumer's O(1) "nothing to do here"
    };

    // ---- sculpt brushes (pure sample math; the editor sculpt tool wraps these) ------------------
    //
    // Each brush edits the samples under a world-space disc (center worldX, worldZ; radius) with a
    // cosine falloff (1 at the center, 0 at the rim), clamps to the u16 range, calls BumpVersion()
    // if any sample changed, and returns the touched grid rectangle (inclusive): the region a
    // stroke's undo command snapshots and the renderer's GPU re-upload can bound to.

    /// The grid rectangle a brush touched (inclusive). Empty (IsEmpty) when nothing was in range.
    struct HeightfieldRegion
    {
        i32 minX = 0x7fffffff;
        i32 minZ = 0x7fffffff;
        i32 maxX = -1;
        i32 maxZ = -1;

        [[nodiscard]] bool IsEmpty() const noexcept { return maxX < minX || maxZ < minZ; }
        [[nodiscard]] i32 Width() const noexcept { return IsEmpty() ? 0 : (maxX - minX + 1); }
        [[nodiscard]] i32 Height() const noexcept { return IsEmpty() ? 0 : (maxZ - minZ + 1); }
        void Add(i32 gx, i32 gz) noexcept
        {
            minX = gx < minX ? gx : minX;
            minZ = gz < minZ ? gz : minZ;
            maxX = gx > maxX ? gx : maxX;
            maxZ = gz > maxZ ? gz : maxZ;
        }
    };

    namespace detail
    {
        // The grid rect (clamped) covering a world disc, and a visit of every in-disc cell with its
        // cosine falloff weight. `apply(gx, gz, w)` mutates one sample; visit tracks the touched rect.
        template <typename Apply>
        inline HeightfieldRegion VisitBrush(const Heightfield& hf, f32 worldX, f32 worldZ, f32 radius,
                                            Apply&& apply)
        {
            HeightfieldRegion region;
            if (hf.IsEmpty() || radius <= 0.0f)
            {
                return region;
            }
            const i32 n = hf.Size();
            const Float2 ws = hf.WorldSize();
            const f32 spanX = ws.x / static_cast<f32>(n - 1); // world units per cell
            const f32 spanZ = ws.y / static_cast<f32>(n - 1);
            const Float2 c = hf.WorldToGrid(worldX, worldZ);
            const f32 gRadX = radius / (spanX > 1.0e-6f ? spanX : 1.0f);
            const f32 gRadZ = radius / (spanZ > 1.0e-6f ? spanZ : 1.0f);
            const i32 x0 = Max(0, static_cast<i32>(Floor(c.x - gRadX)));
            const i32 x1 = Min(n - 1, static_cast<i32>(Ceil(c.x + gRadX)));
            const i32 z0 = Max(0, static_cast<i32>(Floor(c.y - gRadZ)));
            const i32 z1 = Min(n - 1, static_cast<i32>(Ceil(c.y + gRadZ)));
            const f32 invRadius = 1.0f / radius;
            for (i32 gz = z0; gz <= z1; ++gz)
            {
                for (i32 gx = x0; gx <= x1; ++gx)
                {
                    const Float2 wp = hf.GridToWorld(static_cast<f32>(gx), static_cast<f32>(gz));
                    const f32 dx = wp.x - worldX;
                    const f32 dz = wp.y - worldZ;
                    const f32 dist = Sqrt(dx * dx + dz * dz);
                    if (dist >= radius)
                    {
                        continue;
                    }
                    const f32 w = 0.5f + 0.5f * Cos(3.14159265f * dist * invRadius); // 1 center..0 rim
                    apply(gx, gz, w);
                    region.Add(gx, gz);
                }
            }
            return region;
        }

        [[nodiscard]] inline Height ClampSample(f32 v) noexcept
        {
            const f32 c = v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v);
            return static_cast<Height>(c + 0.5f);
        }
    }

    /// Raise (positive strength) or lower (negative) the surface by up to `strengthWorldY` world
    /// units at the brush center, falling off to the rim.
    inline HeightfieldRegion SculptRaise(Heightfield& hf, f32 worldX, f32 worldZ, f32 radius,
                                         f32 strengthWorldY)
    {
        const f32 range = hf.MaxY() - hf.MinY();
        const f32 perUnit = (range > 1.0e-6f) ? (65535.0f / range) : 0.0f;
        const HeightfieldRegion region = detail::VisitBrush(
            hf, worldX, worldZ, radius,
            [&](i32 gx, i32 gz, f32 w)
            {
                const f32 s = static_cast<f32>(hf.GetSample(gx, gz)) + strengthWorldY * w * perUnit;
                hf.SetSample(gx, gz, detail::ClampSample(s));
            });
        if (!region.IsEmpty())
        {
            hf.BumpVersion();
        }
        return region;
    }

    /// Pull the surface toward `targetWorldY` by `amount` in [0,1] (scaled by the falloff).
    inline HeightfieldRegion SculptFlatten(Heightfield& hf, f32 worldX, f32 worldZ, f32 radius,
                                           f32 amount, f32 targetWorldY)
    {
        const f32 target = static_cast<f32>(hf.WorldYToSample(targetWorldY));
        const HeightfieldRegion region = detail::VisitBrush(
            hf, worldX, worldZ, radius,
            [&](i32 gx, i32 gz, f32 w)
            {
                const f32 s = static_cast<f32>(hf.GetSample(gx, gz));
                hf.SetSample(gx, gz, detail::ClampSample(s + (target - s) * (w * amount)));
            });
        if (!region.IsEmpty())
        {
            hf.BumpVersion();
        }
        return region;
    }

    /// Smooth the surface toward each cell's 3x3 neighbourhood average by `amount` in [0,1].
    inline HeightfieldRegion SculptSmooth(Heightfield& hf, f32 worldX, f32 worldZ, f32 radius,
                                          f32 amount)
    {
        const i32 n = hf.Size();
        const HeightfieldRegion region = detail::VisitBrush(
            hf, worldX, worldZ, radius,
            [&](i32 gx, i32 gz, f32 w)
            {
                f32 sum = 0.0f;
                i32 count = 0;
                for (i32 dz = -1; dz <= 1; ++dz)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        const i32 nx = gx + dx;
                        const i32 nz = gz + dz;
                        if (nx >= 0 && nx < n && nz >= 0 && nz < n)
                        {
                            sum += static_cast<f32>(hf.GetSample(nx, nz));
                            ++count;
                        }
                    }
                }
                const f32 avg = (count > 0) ? (sum / static_cast<f32>(count)) : 0.0f;
                const f32 s = static_cast<f32>(hf.GetSample(gx, gz));
                hf.SetSample(gx, gz, detail::ClampSample(s + (avg - s) * (w * amount)));
            });
        if (!region.IsEmpty())
        {
            hf.BumpVersion();
        }
        return region;
    }

    RTTI_DEFINE_OBJECT(Heightfield, "rtti::heightfield")
    // ---- hole brushes (the terrain.hole tool) ---------------------------------------------------
    // A sample is cut or solid, so the disc has a hard edge: every sample inside the radius is
    // marked, none outside, no falloff. The version bumps once when anything changed, and the
    // touched rect comes back for the stroke's region-delta undo and the renderer's rebuild.

    namespace detail
    {
        inline HeightfieldRegion MarkHoles(Heightfield& hf, f32 worldX, f32 worldZ, f32 radius,
                                           bool cut)
        {
            bool changed = false;
            const HeightfieldRegion region = VisitBrush(
                hf, worldX, worldZ, radius,
                [&](i32 gx, i32 gz, f32)
                {
                    if (hf.IsHole(gx, gz) != cut)
                    {
                        hf.SetHole(gx, gz, cut);
                        changed = true;
                    }
                });
            if (changed)
            {
                hf.BumpVersion();
            }
            return region;
        }
    }

    /// Cut every sample inside the disc.
    inline HeightfieldRegion CutHoles(Heightfield& hf, f32 worldX, f32 worldZ, f32 radius)
    {
        return detail::MarkHoles(hf, worldX, worldZ, radius, true);
    }

    /// Fill (restore) every sample inside the disc.
    inline HeightfieldRegion FillHoles(Heightfield& hf, f32 worldX, f32 worldZ, f32 radius)
    {
        return detail::MarkHoles(hf, worldX, worldZ, radius, false);
    }

}
