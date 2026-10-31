// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Foundation::Terrain.Resource - :splatmap partition.
///
/// The editable terrain splat weights: the top-K (K = 4) blend model.
/// Per texel, two equal-size rasters hold up to four (paletteIndex, weight) pairs:
///   - indices: 4 x u8 palette indices (0..255); a slot is unused iff its weight is 0.
///   - weights: 4 x u8 quantized weights (0..255 maps to 0..1), sum <= 255.
/// The base layer implicitly owns the remainder (baseW = 1 - sum/255): an all-zero raster is a
/// valid pure-base surface by construction, so nothing needs seeding. This is the CPU source of
/// truth, as the Heightfield is; engine.terrain derives the GPU textures from it, keyed by
/// uid and Version(). The pure brush cores (PaintTopK/EraseTopK) live here: no RHI, headless.
///
/// SplatWeightsSource is the cooked metadata (width and height); the two pixel blobs travel in
/// the `kSplatStream` ("pixels", the weights) and `kSplatIndexStream` ("indices") sidecars, as
/// bulk data does. SplatWeightsFactory builds the runtime SplatWeights. An imported flat RGBA8
/// raster (R the base share, G/B/A palette 0..2) converts through
/// SplatWeightsFromFixedLayerRaster, renormalized by the texel sum so the shares are exact.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.terrain.resource:splatmap;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::terrain
{
    /// Layers blended per texel (the K in top-K). The palette itself is unbounded (8-bit index,
    /// up to 256 layers, which is WebGPU's minimum maxTextureArrayLayers; do not widen the index
    /// without checking that limit).
    inline constexpr u32 kSplatSlotCount = 4;

    /// The per-texel top-K (index, weight) rasters over the terrain's 0..1 footprint UV. It is an
    /// Object so it is a referenceable product (Ref<SplatWeights>) the terrain holds. The
    /// per-object uid and Version() drive the GPU cache invalidation, as the Heightfield's do.
    class SplatWeights final : public Object
    {
        RTTI_OBJECT(SplatWeights, Object)
    public:
        // Unique per-object id: GPU caches key by this and Version(), never by pointer, because a
        // freed raster's address can be reused by a fresh one at an equal version (the same rule
        // as the height-texture cache).
        const u64 uid = NextUid();

        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        SplatWeights() = default;

        /// Allocate an all-zero pair of rasters: every weight 0, so pure base everywhere (the base
        /// is a separate layer, so a fresh weights raster needs no seeding).
        SplatWeights(i32 width, i32 height) : m_width(width), m_height(height)
        {
            const usize bytes =
                static_cast<usize>(width) * static_cast<usize>(height) * kSplatSlotCount;
            m_indices.Resize(bytes, u8{0});
            m_weights.Resize(bytes, u8{0});
        }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_width <= 0 || m_height <= 0; }
        [[nodiscard]] i32 Width() const noexcept { return m_width; }
        [[nodiscard]] i32 Height() const noexcept { return m_height; }

        /// Monotonic edit generation (starts at 1). Bump after a paint so the GPU splat textures
        /// re-upload (the sculpt and regen path).
        [[nodiscard]] u64 Version() const noexcept { return m_version; }
        void BumpVersion() noexcept { ++m_version; }

        [[nodiscard]] Span<const u8> Indices() const noexcept
        {
            return Span<const u8>{m_indices.Data(), m_indices.Size()};
        }
        [[nodiscard]] Span<u8> Indices() noexcept
        {
            return Span<u8>{m_indices.Data(), m_indices.Size()};
        }
        [[nodiscard]] Span<const u8> Weights() const noexcept
        {
            return Span<const u8>{m_weights.Data(), m_weights.Size()};
        }
        [[nodiscard]] Span<u8> Weights() noexcept
        {
            return Span<u8>{m_weights.Data(), m_weights.Size()};
        }

        /// Slot accessors (texel clamped, slot masked). A slot is meaningful iff its weight > 0.
        [[nodiscard]] u8 SlotIndex(i32 x, i32 y, u32 slot) const noexcept
        {
            return m_indices[TexelOffset(x, y) + (slot & 3u)];
        }
        [[nodiscard]] u8 SlotWeight(i32 x, i32 y, u32 slot) const noexcept
        {
            return m_weights[TexelOffset(x, y) + (slot & 3u)];
        }

        /// The weight (0..255) this texel gives palette layer `paletteIndex` (0 if not in a slot).
        [[nodiscard]] u8 WeightOfLayer(i32 x, i32 y, u32 paletteIndex) const noexcept
        {
            const usize at = TexelOffset(x, y);
            for (u32 k = 0; k < kSplatSlotCount; ++k)
            {
                if (m_weights[at + k] > 0 && m_indices[at + k] == paletteIndex)
                {
                    return m_weights[at + k];
                }
            }
            return 0;
        }

        /// The implicit base weight (0..255) at a texel: 255 - sum(slot weights), clamped.
        [[nodiscard]] u8 BaseWeight(i32 x, i32 y) const noexcept
        {
            const usize at = TexelOffset(x, y);
            i32 sum = 0;
            for (u32 k = 0; k < kSplatSlotCount; ++k)
            {
                sum += m_weights[at + k];
            }
            return static_cast<u8>(sum >= 255 ? 0 : 255 - sum);
        }

        [[nodiscard]] usize TexelOffset(i32 x, i32 y) const noexcept
        {
            const i32 cx = x < 0 ? 0 : (x >= m_width ? m_width - 1 : x);
            const i32 cy = y < 0 ? 0 : (y >= m_height ? m_height - 1 : y);
            return (static_cast<usize>(cy) * static_cast<usize>(m_width) + static_cast<usize>(cx)) *
                   kSplatSlotCount;
        }

    private:
        i32 m_width = 0;
        i32 m_height = 0;
        u64 m_version = 1;  // edit generation (BumpVersion) for GPU-cache invalidation
        Array<u8> m_indices; // 4 x u8 palette indices per texel, row-major
        Array<u8> m_weights; // 4 x u8 weights per texel, row-major, sum <= 255
    };

    /// The pixel rectangle a paint touched (inclusive). Empty when nothing was in range.
    struct SplatRegion
    {
        i32 minX = 0x7fffffff;
        i32 minY = 0x7fffffff;
        i32 maxX = -1;
        i32 maxY = -1;

        [[nodiscard]] bool IsEmpty() const noexcept { return maxX < minX || maxY < minY; }
        [[nodiscard]] i32 Width() const noexcept { return IsEmpty() ? 0 : (maxX - minX + 1); }
        [[nodiscard]] i32 Height() const noexcept { return IsEmpty() ? 0 : (maxY - minY + 1); }
        void Add(i32 x, i32 y) noexcept
        {
            minX = x < minX ? x : minX;
            minY = y < minY ? y : minY;
            maxX = x > maxX ? x : maxX;
            maxY = y > maxY ? y : maxY;
        }
    };

    namespace detail
    {
        // Shared elliptical-brush visitor: calls fn(x, y, t) for every texel inside the world
        // circle (per-axis UV radii, the same non-square-footprint handling the sculpt brush
        // uses) with the falloff-scaled strength t in (0, 1]. `coreFraction` is the flat inner
        // core's share of the radius (full strength inside it, cosine skirt outside).
        template <typename TFn>
        inline void VisitSplatBrush(const SplatWeights& sw, f32 uvX, f32 uvY, f32 uvRadiusX,
                                    f32 uvRadiusY, f32 amount, f32 coreFraction, TFn&& fn)
        {
            const i32 w = sw.Width();
            const i32 h = sw.Height();
            const f32 cx = uvX * static_cast<f32>(w);
            const f32 cy = uvY * static_cast<f32>(h);
            const f32 rx = uvRadiusX * static_cast<f32>(w);
            const f32 ry = uvRadiusY * static_cast<f32>(h);
            const i32 x0 = Max(0, static_cast<i32>(Floor(cx - rx)));
            const i32 x1 = Min(w - 1, static_cast<i32>(Ceil(cx + rx)));
            const i32 y0 = Max(0, static_cast<i32>(Floor(cy - ry)));
            const i32 y1 = Min(h - 1, static_cast<i32>(Ceil(cy + ry)));
            const f32 invRx = 1.0f / uvRadiusX;
            const f32 invRy = 1.0f / uvRadiusY;
            for (i32 y = y0; y <= y1; ++y)
            {
                for (i32 x = x0; x <= x1; ++x)
                {
                    const f32 du =
                        ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(w) - uvX) * invRx;
                    const f32 dv =
                        ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(h) - uvY) * invRy;
                    const f32 dist = Sqrt(du * du + dv * dv); // 0 centre .. 1 rim (world circle)
                    if (dist >= 1.0f)
                    {
                        continue;
                    }
                    // Hard inner core and cosine skirt: full strength inside `core`, the smooth
                    // falloff to the rim outside it. The core makes a full-strength stamp
                    // decisive (one-hot paint or a clean erase at the texel under the cursor):
                    // the stamp-brush model needs an exact 1 somewhere, and a pure cosine never
                    // delivers it at a texel centre. The skirt keeps the edge soft. Callers
                    // scale the core with the stamp amount (soft profile at blending strengths,
                    // hard at 1.0); see the paint tool.
                    const f32 core = Clamp(coreFraction, 0.0f, 0.95f);
                    const f32 fall =
                        dist <= core
                            ? 1.0f
                            : 0.5f + 0.5f * Cos(3.14159265f * (dist - core) / (1.0f - core));
                    const f32 t = Clamp(amount * fall, 0.0f, 1.0f);
                    if (t > 0.0f)
                    {
                        fn(x, y, t);
                    }
                }
            }
        }
    }

    /// Paint palette layer `paletteIndex` over a world-space brush disc (per-axis UV radii,
    /// centre uvX/uvY, strength `amount` in [0,1]). Per touched texel, the top-K update:
    ///   1. slot = the slot already holding L, else a free (weight-0) slot, else evict the
    ///      minimum-weight slot unconditionally (index = L, weight = 0; the evicted weight falls
    ///      to base, so the error is bounded by the smallest weight, the least visible).
    ///   2. every other slot fades: w *= (1 - t) (base fades too, since base = 1 - sum).
    ///   3. L raises: w = w + t * (1 - w).
    /// The result stays convex (sum' = lerp(sum, 1, t) <= 1); repeated full-strength painting
    /// drives the texel to pure L. Weights that quantize to 0 free their slot (index cleared) so
    /// the top-K stays meaningful. Bumps the version if anything changed; returns the touched
    /// rect for region-delta undo and the bounded GPU re-upload.
    inline SplatRegion PaintTopK(SplatWeights& sw, f32 uvX, f32 uvY, f32 uvRadiusX, f32 uvRadiusY,
                                 u32 paletteIndex, f32 amount, f32 coreFraction = 0.5f)
    {
        SplatRegion region;
        if (sw.IsEmpty() || uvRadiusX <= 0.0f || uvRadiusY <= 0.0f || amount <= 0.0f ||
            paletteIndex > 255u)
        {
            return region;
        }
        Span<u8> idx = sw.Indices();
        Span<u8> wts = sw.Weights();
        const u8 layer = static_cast<u8>(paletteIndex);
        bool changed = false;
        detail::VisitSplatBrush(
            sw, uvX, uvY, uvRadiusX, uvRadiusY, amount, coreFraction,
            [&](i32 x, i32 y, f32 t)
            {
                const usize at = sw.TexelOffset(x, y);

                // 1. Choose L's slot: the existing one, else a free one, else evict the minimum.
                u32 slot = kSplatSlotCount;
                for (u32 k = 0; k < kSplatSlotCount; ++k)
                {
                    if (wts[at + k] > 0 && idx[at + k] == layer)
                    {
                        slot = k;
                        break;
                    }
                }
                if (slot == kSplatSlotCount)
                {
                    for (u32 k = 0; k < kSplatSlotCount; ++k)
                    {
                        if (wts[at + k] == 0)
                        {
                            slot = k;
                            break;
                        }
                    }
                }
                if (slot == kSplatSlotCount)
                {
                    u32 minSlot = 0;
                    for (u32 k = 1; k < kSplatSlotCount; ++k)
                    {
                        if (wts[at + k] < wts[at + minSlot])
                        {
                            minSlot = k;
                        }
                    }
                    slot = minSlot;
                    wts[at + slot] = 0; // evicted weight falls to base (sum drops)
                }

                // 2 and 3. Fade others, raise L; compute in float, quantize once. The raised slot
                // quantizes last, capped by the others' quantized sum: per-slot round-to-nearest
                // can otherwise push the u8 sum one over 255 (e.g. t = 0.5 rounds both halves
                // up), and convexity (sum + base == 1) must hold exactly in the stored bytes.
                bool texelChanged = false;
                i32 othersSum = 0;
                for (u32 k = 0; k < kSplatSlotCount; ++k)
                {
                    if (k == slot)
                    {
                        continue;
                    }
                    const f32 wv = static_cast<f32>(wts[at + k]) * (1.0f / 255.0f);
                    const f32 nv = wv * (1.0f - t);
                    const u8 q = static_cast<u8>(Clamp(nv * 255.0f + 0.5f, 0.0f, 255.0f));
                    if (q != wts[at + k])
                    {
                        wts[at + k] = q;
                        texelChanged = true;
                    }
                    othersSum += q;
                }
                {
                    const f32 wv = static_cast<f32>(wts[at + slot]) * (1.0f / 255.0f);
                    const f32 nv = wv + t * (1.0f - wv);
                    const f32 cap = static_cast<f32>(255 - Min(othersSum, 255));
                    const u8 q = static_cast<u8>(Clamp(nv * 255.0f + 0.5f, 0.0f, cap));
                    if (q != wts[at + slot])
                    {
                        wts[at + slot] = q;
                        texelChanged = true;
                    }
                }
                if (idx[at + slot] != layer)
                {
                    idx[at + slot] = layer;
                    texelChanged = true;
                }
                // Freed slots (quantized to 0) clear their index so the top-K stays meaningful.
                for (u32 k = 0; k < kSplatSlotCount; ++k)
                {
                    if (wts[at + k] == 0 && idx[at + k] != 0)
                    {
                        idx[at + k] = 0;
                        texelChanged = true;
                    }
                }
                if (texelChanged)
                {
                    region.Add(x, y);
                    changed = true;
                }
            });
        if (changed)
        {
            sw.BumpVersion();
        }
        return region;
    }

    /// Erase over the brush disc: every slot fades w *= (1 - t), so the base (the remainder)
    /// rises toward 1. Slots that quantize to 0 are freed (index cleared).
    inline SplatRegion EraseTopK(SplatWeights& sw, f32 uvX, f32 uvY, f32 uvRadiusX, f32 uvRadiusY,
                                 f32 amount, f32 coreFraction = 0.5f)
    {
        SplatRegion region;
        if (sw.IsEmpty() || uvRadiusX <= 0.0f || uvRadiusY <= 0.0f || amount <= 0.0f)
        {
            return region;
        }
        Span<u8> idx = sw.Indices();
        Span<u8> wts = sw.Weights();
        bool changed = false;
        detail::VisitSplatBrush(
            sw, uvX, uvY, uvRadiusX, uvRadiusY, amount, coreFraction,
            [&](i32 x, i32 y, f32 t)
            {
                const usize at = sw.TexelOffset(x, y);
                bool texelChanged = false;
                for (u32 k = 0; k < kSplatSlotCount; ++k)
                {
                    const f32 wv = static_cast<f32>(wts[at + k]) * (1.0f / 255.0f);
                    const u8 q =
                        static_cast<u8>(Clamp(wv * (1.0f - t) * 255.0f + 0.5f, 0.0f, 255.0f));
                    if (q != wts[at + k])
                    {
                        wts[at + k] = q;
                        texelChanged = true;
                    }
                    if (wts[at + k] == 0 && idx[at + k] != 0)
                    {
                        idx[at + k] = 0;
                        texelChanged = true;
                    }
                }
                if (texelChanged)
                {
                    region.Add(x, y);
                    changed = true;
                }
            });
        if (changed)
        {
            sw.BumpVersion();
        }
        return region;
    }

    /// Smooth (blur) over the brush disc: every texel's weights move toward the average of their
    /// 3x3 neighborhood (clamped at the raster edge), `w = lerp(w, avg, t)`, feathering an
    /// already-painted seam without repainting either side. The base needs no handling: it is the
    /// remainder, and an average of convex texels stays convex. Neighborhoods are read from a
    /// snapshot of the touched rect (plus a 1-texel ring) so the pass is order-independent, with no
    /// directional smearing from reading already-smoothed texels. Where the union of neighborhood
    /// layers exceeds K, the K largest smoothed weights win (the dropped tail falls to base, the
    /// same least-visible-error rule as paint eviction). Bumps the version if anything changed;
    /// returns the touched rect for region-delta undo and the bounded GPU re-upload.
    inline SplatRegion SmoothTopK(SplatWeights& sw, f32 uvX, f32 uvY, f32 uvRadiusX, f32 uvRadiusY,
                                  f32 amount, f32 coreFraction = 0.5f)
    {
        SplatRegion region;
        if (sw.IsEmpty() || uvRadiusX <= 0.0f || uvRadiusY <= 0.0f || amount <= 0.0f)
        {
            return region;
        }
        const i32 w = sw.Width();
        const i32 h = sw.Height();
        // Snapshot the brush rect and a 1-texel ring (every neighborhood read lands inside it).
        const f32 cx = uvX * static_cast<f32>(w);
        const f32 cy = uvY * static_cast<f32>(h);
        const f32 rx = uvRadiusX * static_cast<f32>(w);
        const f32 ry = uvRadiusY * static_cast<f32>(h);
        const i32 sx0 = Max(0, static_cast<i32>(Floor(cx - rx)) - 1);
        const i32 sx1 = Min(w - 1, static_cast<i32>(Ceil(cx + rx)) + 1);
        const i32 sy0 = Max(0, static_cast<i32>(Floor(cy - ry)) - 1);
        const i32 sy1 = Min(h - 1, static_cast<i32>(Ceil(cy + ry)) + 1);
        if (sx1 < sx0 || sy1 < sy0)
        {
            return region;
        }
        const i32 snapW = sx1 - sx0 + 1;
        const i32 snapH = sy1 - sy0 + 1;
        const usize snapBytes =
            static_cast<usize>(snapW) * static_cast<usize>(snapH) * kSplatSlotCount;
        Array<u8> snapIdx;
        Array<u8> snapWts;
        snapIdx.Resize(snapBytes);
        snapWts.Resize(snapBytes);
        {
            const Span<const u8> idx = sw.Indices();
            const Span<const u8> wts = sw.Weights();
            for (i32 y = sy0; y <= sy1; ++y)
            {
                const usize src = sw.TexelOffset(sx0, y);
                const usize dst = static_cast<usize>(y - sy0) * static_cast<usize>(snapW) *
                                  kSplatSlotCount;
                const usize rowBytes = static_cast<usize>(snapW) * kSplatSlotCount;
                MemCopy(snapIdx.Data() + dst, idx.Data() + src, rowBytes);
                MemCopy(snapWts.Data() + dst, wts.Data() + src, rowBytes);
            }
        }
        const auto snapAt = [&](i32 x, i32 y) -> usize
        {
            return (static_cast<usize>(y - sy0) * static_cast<usize>(snapW) +
                    static_cast<usize>(x - sx0)) *
                   kSplatSlotCount;
        };

        Span<u8> idx = sw.Indices();
        Span<u8> wts = sw.Weights();
        bool changed = false;
        detail::VisitSplatBrush(
            sw, uvX, uvY, uvRadiusX, uvRadiusY, amount, coreFraction,
            [&](i32 x, i32 y, f32 t)
            {
                // Accumulate per-layer weight sums over the clamped 3x3 neighborhood (edge
                // texels clamp-extend). Union of layers across 9 texels: at most 36 candidates.
                u8 candIdx[9 * kSplatSlotCount];
                f32 candSum[9 * kSplatSlotCount];
                u32 candCount = 0;
                for (i32 dy = -1; dy <= 1; ++dy)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        const i32 nx = Clamp(x + dx, sx0, sx1);
                        const i32 ny = Clamp(y + dy, sy0, sy1);
                        const usize at = snapAt(nx, ny);
                        for (u32 k = 0; k < kSplatSlotCount; ++k)
                        {
                            const u8 nw = snapWts[at + k];
                            if (nw == 0)
                            {
                                continue;
                            }
                            const u8 layer = snapIdx[at + k];
                            u32 c = 0;
                            for (; c < candCount; ++c)
                            {
                                if (candIdx[c] == layer)
                                {
                                    break;
                                }
                            }
                            if (c == candCount)
                            {
                                candIdx[candCount] = layer;
                                candSum[candCount] = 0.0f;
                                ++candCount;
                            }
                            candSum[c] += static_cast<f32>(nw) * (1.0f / 255.0f);
                        }
                    }
                }

                // Target per layer = lerp(own weight, neighborhood average, t).
                const usize own = snapAt(x, y);
                f32 candNew[9 * kSplatSlotCount];
                for (u32 c = 0; c < candCount; ++c)
                {
                    f32 cur = 0.0f;
                    for (u32 k = 0; k < kSplatSlotCount; ++k)
                    {
                        if (snapWts[own + k] > 0 && snapIdx[own + k] == candIdx[c])
                        {
                            cur = static_cast<f32>(snapWts[own + k]) * (1.0f / 255.0f);
                            break;
                        }
                    }
                    const f32 avg = candSum[c] * (1.0f / 9.0f);
                    candNew[c] = cur + t * (avg - cur);
                }

                // Keep the K largest smoothed weights (selection sort; K is 4). Quantize in
                // descending order, each capped by the remaining byte budget, so convexity
                // (sum <= 255) holds exactly and rounding error lands on the smallest weights.
                const usize at = sw.TexelOffset(x, y);
                u8 newIdx[kSplatSlotCount] = {};
                u8 newWts[kSplatSlotCount] = {};
                i32 budget = 255;
                for (u32 k = 0; k < kSplatSlotCount && k < candCount; ++k)
                {
                    u32 best = k;
                    for (u32 c = k + 1; c < candCount; ++c)
                    {
                        if (candNew[c] > candNew[best])
                        {
                            best = c;
                        }
                    }
                    const u8 bi = candIdx[best];
                    const f32 bw = candNew[best];
                    candIdx[best] = candIdx[k];
                    candNew[best] = candNew[k];
                    candIdx[k] = bi;
                    candNew[k] = bw;
                    const u8 q = static_cast<u8>(
                        Clamp(bw * 255.0f + 0.5f, 0.0f, static_cast<f32>(budget)));
                    if (q == 0)
                    {
                        break;
                    }
                    newIdx[k] = bi;
                    newWts[k] = q;
                    budget -= q;
                }
                bool texelChanged = false;
                for (u32 k = 0; k < kSplatSlotCount; ++k)
                {
                    if (idx[at + k] != newIdx[k] || wts[at + k] != newWts[k])
                    {
                        idx[at + k] = newIdx[k];
                        wts[at + k] = newWts[k];
                        texelChanged = true;
                    }
                }
                if (texelChanged)
                {
                    region.Add(x, y);
                    changed = true;
                }
            });
        if (changed)
        {
            sw.BumpVersion();
        }
        return region;
    }

    /// Palette-remove remap: slots referencing `removedIndex` are freed (their weight
    /// falls to the base) and indices above it decrement so every surviving slot still names its
    /// layer. Whole-raster; bumps the version when anything changed. Returns whether it did.
    inline bool RemapOnPaletteRemove(SplatWeights& sw, u32 removedIndex)
    {
        if (sw.IsEmpty())
        {
            return false;
        }
        Span<u8> idx = sw.Indices();
        Span<u8> wts = sw.Weights();
        bool changed = false;
        for (usize i = 0; i < idx.Size(); ++i)
        {
            if (wts[i] == 0)
            {
                continue;
            }
            if (idx[i] == removedIndex)
            {
                wts[i] = 0; // freed weight falls to base
                idx[i] = 0;
                changed = true;
            }
            else if (idx[i] > removedIndex)
            {
                idx[i] = static_cast<u8>(idx[i] - 1);
                changed = true;
            }
        }
        if (changed)
        {
            sw.BumpVersion();
        }
        return changed;
    }

    /// Convert an imported flat raster (RGBA8, four fixed layers: R = the base share, G/B/A =
    /// palette 0,1,2) to the top-K model. The channels are renormalized by the texel
    /// sum, so a raster whose sums drift from 255 still yields the exact shares (the base is the
    /// deficit, 255 - the palette slots). A zero-sum texel becomes pure base.
    [[nodiscard]] inline RefPtr<SplatWeights> SplatWeightsFromFixedLayerRaster(
        Span<const u8> rgba, i32 width, i32 height, IAllocator& allocator)
    {
        if (width <= 0 || height <= 0 ||
            rgba.Size() != static_cast<usize>(width) * static_cast<usize>(height) * 4u)
        {
            return MakeRef<SplatWeights>(allocator);
        }
        RefPtr<SplatWeights> sw = MakeRef<SplatWeights>(allocator, width, height);
        Span<u8> idx = sw->Indices();
        Span<u8> wts = sw->Weights();
        const usize texels = static_cast<usize>(width) * static_cast<usize>(height);
        for (usize i = 0; i < texels; ++i)
        {
            const usize at = i * 4u;
            const f32 w0 = static_cast<f32>(rgba[at + 0]);
            const f32 w1 = static_cast<f32>(rgba[at + 1]);
            const f32 w2 = static_cast<f32>(rgba[at + 2]);
            const f32 w3 = static_cast<f32>(rgba[at + 3]);
            const f32 sum = w0 + w1 + w2 + w3;
            if (sum <= 0.0f)
            {
                continue; // all-zero slots: pure base
            }
            // Channels G/B/A -> palette indices 0..2, renormalized by the texel sum.
            const f32 scale = 255.0f / sum;
            idx[at + 0] = 0;
            idx[at + 1] = 1;
            idx[at + 2] = 2;
            idx[at + 3] = 0;
            wts[at + 0] = static_cast<u8>(Clamp(w1 * scale + 0.5f, 0.0f, 255.0f));
            wts[at + 1] = static_cast<u8>(Clamp(w2 * scale + 0.5f, 0.0f, 255.0f));
            wts[at + 2] = static_cast<u8>(Clamp(w3 * scale + 0.5f, 0.0f, 255.0f));
            wts[at + 3] = 0;
            // Zero slots clear their index (weight 0 is unused); base = 255 - sum, w0's share.
            for (u32 k = 0; k < kSplatSlotCount; ++k)
            {
                if (wts[at + k] == 0)
                {
                    idx[at + k] = 0;
                }
            }
        }
        return sw;
    }

    /// Cooked splat-weights metadata: just the raster dimensions. The two pixel blobs are not
    /// here; they travel in the `kSplatStream` (weights) and `kSplatIndexStream` (indices) sidecars.
    class SplatWeightsSource final : public ISerializable
    {
        RTTI_OBJECT(SplatWeightsSource, ISerializable)
    public:
        i32 width = 0;
        i32 height = 0;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
        }

        static void FromWeights(const SplatWeights& sw, SplatWeightsSource& out)
        {
            out.width = sw.Width();
            out.height = sw.Height();
        }

        [[nodiscard]] static Span<const byte> WeightBlob(const SplatWeights& sw) noexcept
        {
            const Span<const u8> px = sw.Weights();
            return Span<const byte>(reinterpret_cast<const byte*>(px.Data()), px.Size());
        }
        [[nodiscard]] static Span<const byte> IndexBlob(const SplatWeights& sw) noexcept
        {
            const Span<const u8> px = sw.Indices();
            return Span<const byte>(reinterpret_cast<const byte*>(px.Data()), px.Size());
        }

        /// Build the runtime product from this metadata and the two sidecar blobs. Inconsistent
        /// data (bad dims, blob size mismatch) yields an empty raster rather than a malformed one.
        [[nodiscard]] RefPtr<SplatWeights> Build(Span<const byte> indexBlob,
                                                 Span<const byte> weightBlob,
                                                 IAllocator& allocator) const
        {
            if (width <= 0 || height <= 0)
            {
                return MakeRef<SplatWeights>(allocator);
            }
            const usize expected =
                static_cast<usize>(width) * static_cast<usize>(height) * kSplatSlotCount;
            if (indexBlob.Size() != expected || weightBlob.Size() != expected)
            {
                return MakeRef<SplatWeights>(allocator);
            }
            RefPtr<SplatWeights> sw = MakeRef<SplatWeights>(allocator, width, height);
            MemCopy(sw->Indices().Data(), indexBlob.Data(), expected);
            MemCopy(sw->Weights().Data(), weightBlob.Data(), expected);
            return sw;
        }
    };

    /// Sidecar stream names: `kSplatStream` carries the weight raster, `kSplatIndexStream` the
    /// palette-index raster. Both are required; a cooked instance missing either fails to load.
    inline constexpr StringView kSplatStream = u8"pixels";
    inline constexpr StringView kSplatIndexStream = u8"indices";

    /// Builds cooked splat weights (metadata and two streams) into a runtime SplatWeights.
    /// Pure-CPU, async-safe.
    class SplatWeightsFactory final : public IResourceFactory
    {
    public:
        // The allocator backs every product this factory creates (required;
        // the application that registers the factory decides).
        explicit SplatWeightsFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &SplatWeights::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &SplatWeightsSource::StaticType();
        }
        [[nodiscard]] RefPtr<Object> Create(ResourceManager&,
                                            foundation::content::Instance& instance) override
        {
            return BuildFrom(instance);
        }
        [[nodiscard]] bool SupportsAsync() const override { return true; }
        [[nodiscard]] RefPtr<Object> DecodeStage(foundation::content::Instance& instance) override
        {
            return BuildFrom(instance);
        }
        [[nodiscard]] RefPtr<Object> FinalizeStage(ResourceManager&, RefPtr<Object> decoded) override
        {
            return decoded;
        }

    private:
        [[nodiscard]] static Array<u8> ReadBlob(foundation::content::Instance& instance,
                                                StringView stream)
        {
            Array<u8> blob;
            if (UniquePtr<IStream> s = instance.ReadData(stream))
            {
                const i64 size = s->Size();
                if (size > 0)
                {
                    blob.Resize(static_cast<usize>(size));
                    if (s->Read(blob.Data(), static_cast<u64>(size)) != static_cast<u64>(size))
                    {
                        blob.Clear();
                    }
                }
            }
            return blob;
        }

        [[nodiscard]] RefPtr<Object> BuildFrom(foundation::content::Instance& instance) const
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            SplatWeightsSource* src = Cast<SplatWeightsSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            const Array<u8> weights = ReadBlob(instance, kSplatStream);
            const Array<u8> indices = ReadBlob(instance, kSplatIndexStream);
            return src->Build(
                Span<const byte>{reinterpret_cast<const byte*>(indices.Data()), indices.Size()},
                Span<const byte>{reinterpret_cast<const byte*>(weights.Data()), weights.Size()},
                *m_allocator);
        }
    
    private:
        IAllocator* m_allocator;
    };

    /// Register the splat-weights resource types (product and cooked source) for load.
    inline void RegisterSplatmapResourceTypes()
    {
        GlobalTypeRegistry().Register(SplatWeights::StaticType());
        GlobalTypeRegistry().Register(SplatWeightsSource::StaticType());
        RegisterSerializable<SplatWeightsSource>();
    }

    RTTI_DEFINE_OBJECT(SplatWeights, "rtti::terrain")
    RTTI_DEFINE_OBJECT_VERSIONED(SplatWeightsSource, "rtti::terrain", 1)
}
