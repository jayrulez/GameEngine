// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::Terrain - the `:splattexture` partition.
//
// The GPU splat textures for the top-K blend: the CPU SplatWeights (the painted source of truth)
// derives two textures per resource:
//   - weights: RGBA8Unorm (the 4 slot weights; filterable, though the top-K PS loads it for its
//     manual bilinear),
//   - indices: RGBA8Uint (the 4 palette indices; integer and non-filterable, because filtering
//     indices interpolates layer ids into garbage; load-only in the shader).
// Cached by the resource uid and version, as the height-texture cache is: two terrains sharing one
// weights raster share one texture pair, and a paint (a version bump) retires the old pair and
// re-uploads. The new view ids make the set-3 bind cache rebuild with no extra work (the live
// repaint path). Both are single-mip.

module;
#include "Core/Prelude.h"

export module engine.terrain:splattexture;

import foundation.core;
import foundation.rhi;
import foundation.render;           // GpuRetireQueue (in-flight-safe destruction)
import foundation.terrain.resource; // SplatWeights (the CPU top-K rasters)

using namespace foundation::core;

export namespace engine::terrain
{
    namespace rhi = foundation::rhi;
    namespace render = foundation::render;

    /// The derived GPU views for one SplatWeights resource (null when unresolved).
    struct SplatTextureViews
    {
        rhi::TextureView* weightView = nullptr; // RGBA8Unorm
        rhi::TextureView* indexView = nullptr;  // RGBA8Uint (Load-only)
    };

    /// Owns and caches the per-resource GPU texture pairs, keyed by resource uid and version.
    class TerrainSplatTextureCache
    {
    public:
        TerrainSplatTextureCache() = default;
        TerrainSplatTextureCache(const TerrainSplatTextureCache&) = delete;
        TerrainSplatTextureCache& operator=(const TerrainSplatTextureCache&) = delete;

        /// In-flight-safe destruction (wired by the subsystem): a paint's version-bump rebuild
        /// retires the old textures and views through the frame-aged queue, because a submitted
        /// frame still samples the old views through the renderer's set-3 bind group. Unwired
        /// (headless, Null device), it destroys directly.
        void SetRetireQueue(render::GpuRetireQueue* retire) noexcept { m_retire = retire; }

        /// The texture pair for `sw` at `version`. Keyed by the resource's uid, never its pointer:
        /// a freed raster's address can be reused by a fresh one at an equal version. Creates,
        /// uploads and caches on a miss; a stale version for the same resource rebuilds in place
        /// (the paint re-upload path). Null views on a device failure or an empty raster.
        [[nodiscard]] SplatTextureViews GetOrCreate(rhi::Device& device,
                                                    const foundation::terrain::SplatWeights& sw,
                                                    u64 version)
        {
            if (sw.IsEmpty())
            {
                return SplatTextureViews{};
            }
            for (Entry& entry : m_entries)
            {
                if (entry.key == sw.uid)
                {
                    if (entry.version == version && entry.weightView != nullptr)
                    {
                        return SplatTextureViews{entry.weightView, entry.indexView};
                    }
                    RetireOrDestroy(device, entry); // stale version: rebuild in place
                    if (!Build(device, sw, entry))
                    {
                        return SplatTextureViews{};
                    }
                    entry.version = version;
                    return SplatTextureViews{entry.weightView, entry.indexView};
                }
            }
            Entry fresh;
            fresh.key = sw.uid;
            fresh.version = version;
            if (!Build(device, sw, fresh))
            {
                return SplatTextureViews{};
            }
            m_entries.PushBack(fresh);
            const Entry& stored = m_entries[m_entries.Size() - 1];
            return SplatTextureViews{stored.weightView, stored.indexView};
        }

        /// Drops every entry. This runs at scene destroy as well as at shutdown (stopping
        /// play-in-editor destroys the run's scenes mid frame loop), so a live entry may still
        /// sit in a submitted frame's descriptor set: with a retire queue wired the GPU objects
        /// age past every in-flight frame before they are freed (VUID 01026); without one
        /// (Null-device tests) they are destroyed in place.
        void Clear(rhi::Device& device)
        {
            for (Entry& entry : m_entries)
            {
                RetireOrDestroy(device, entry);
            }
            m_entries.Clear();
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            u64 key = 0; // SplatWeights::uid, never a pointer (address reuse aliases)
            u64 version = 0;
            rhi::Texture* weightTexture = nullptr;
            rhi::TextureView* weightView = nullptr;
            rhi::Texture* indexTexture = nullptr;
            rhi::TextureView* indexView = nullptr;
        };

        [[nodiscard]] static bool BuildOne(rhi::Device& device, u32 w, u32 h,
                                           rhi::TextureFormat format, Span<const u8> pixels,
                                           StringView label, rhi::Texture*& outTexture,
                                           rhi::TextureView*& outView)
        {
            rhi::TextureDesc td{};
            td.format = format;
            td.width = w;
            td.height = h;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = label;
            if (!device.CreateTexture(td, outTexture).IsOk() || outTexture == nullptr)
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = format;
            vd.dimension = rhi::TextureViewDimension::Texture2D;
            if (!device.CreateTextureView(outTexture, vd, outView).IsOk() || outView == nullptr)
            {
                device.DestroyTexture(outTexture);
                outTexture = nullptr;
                return false;
            }
            if (rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* batch = nullptr;
                if (queue->CreateTransferBatch(batch).IsOk() && batch != nullptr)
                {
                    rhi::TextureDataLayout layout{};
                    layout.bytesPerRow = w * 4u;
                    layout.rowsPerImage = h;
                    batch->WriteTexture(outTexture, pixels, layout, rhi::Extent3D{w, h, 1});
                    (void)batch->Submit();
                    queue->DestroyTransferBatch(batch);
                }
            }
            return true;
        }

        [[nodiscard]] static bool Build(rhi::Device& device,
                                        const foundation::terrain::SplatWeights& sw, Entry& out)
        {
            const u32 w = static_cast<u32>(sw.Width());
            const u32 h = static_cast<u32>(sw.Height());
            if (!BuildOne(device, w, h, rhi::TextureFormat::RGBA8Unorm, sw.Weights(),
                          u8"terrain.splat.weights", out.weightTexture, out.weightView))
            {
                return false;
            }
            if (!BuildOne(device, w, h, rhi::TextureFormat::RGBA8Uint, sw.Indices(),
                          u8"terrain.splat.indices", out.indexTexture, out.indexView))
            {
                device.DestroyTextureView(out.weightView);
                device.DestroyTexture(out.weightTexture);
                out.weightView = nullptr;
                out.weightTexture = nullptr;
                return false;
            }
            return true;
        }

        void RetireOrDestroy(rhi::Device& device, Entry& entry)
        {
            if (m_retire != nullptr)
            {
                m_retire->Retire(entry.weightView); // aged past every in-flight frame, then freed
                m_retire->Retire(entry.weightTexture);
                m_retire->Retire(entry.indexView);
                m_retire->Retire(entry.indexTexture);
                entry.weightView = nullptr;
                entry.weightTexture = nullptr;
                entry.indexView = nullptr;
                entry.indexTexture = nullptr;
                return;
            }
            Destroy(device, entry);
        }

        static void Destroy(rhi::Device& device, Entry& entry)
        {
            if (entry.weightView != nullptr)
            {
                device.DestroyTextureView(entry.weightView);
                entry.weightView = nullptr;
            }
            if (entry.weightTexture != nullptr)
            {
                device.DestroyTexture(entry.weightTexture);
                entry.weightTexture = nullptr;
            }
            if (entry.indexView != nullptr)
            {
                device.DestroyTextureView(entry.indexView);
                entry.indexView = nullptr;
            }
            if (entry.indexTexture != nullptr)
            {
                device.DestroyTexture(entry.indexTexture);
                entry.indexTexture = nullptr;
            }
        }

        Array<Entry> m_entries;
        render::GpuRetireQueue* m_retire = nullptr; // borrowed (RenderSubsystem owns and ticks it)
    };

    /// The palette-array GPU views and the per-layer tileScale buffer for one terrain.
    struct PaletteGpu
    {
        rhi::TextureView* arrayView = nullptr;  // albedo Texture2DArray, one slice per palette layer
        rhi::TextureView* normalArrayView = nullptr; // null = no layer used a normal map (dummy binds)
        rhi::TextureView* ormArrayView = nullptr;    // null = no layer used an ORM map
        rhi::TextureView* heightArrayView = nullptr; // null = no layer used a height map (dummy binds)
        rhi::TextureView* maskArrayView = nullptr;   // null = no layer used a coverage mask (opaque)
        rhi::Buffer* tileScaleBuffer = nullptr; // f32[paletteCount]; base tile rides the view UBO
        u64 generation = 0;                     // part of the set-3 cache key
    };

    /// Owns and caches the palette Texture2DArray and tileScale storage buffer per terrain
    /// resource, keyed by TerrainPaletteData::uid and a tile-scale hash (never pointers). A palette
    /// edit re-cooks the terrain into a new TerrainPaletteData (a new uid), which rebuilds; the old
    /// array is retired (in-flight frames still sample it through the old set-3 group).
    class TerrainPaletteTextureCache
    {
    public:
        TerrainPaletteTextureCache() = default;
        TerrainPaletteTextureCache(const TerrainPaletteTextureCache&) = delete;
        TerrainPaletteTextureCache& operator=(const TerrainPaletteTextureCache&) = delete;

        void SetRetireQueue(render::GpuRetireQueue* retire) noexcept { m_retire = retire; }

        [[nodiscard]] PaletteGpu
        GetOrCreate(rhi::Device& device, const foundation::terrain::TerrainPaletteData& data,
                    Span<const f32> paletteTileScales)
        {
            if (!data.IsValid())
            {
                return PaletteGpu{};
            }
            const u64 scaleHash = HashScales(paletteTileScales);
            for (Entry& entry : m_entries)
            {
                if (entry.key == data.uid)
                {
                    if (entry.scaleHash == scaleHash && entry.arrayView != nullptr)
                    {
                        return MakeGpu(entry);
                    }
                    RetireOrDestroy(device, entry);
                    if (!Build(device, data, paletteTileScales, entry))
                    {
                        return PaletteGpu{};
                    }
                    entry.scaleHash = scaleHash;
                    ++entry.generation;
                    return MakeGpu(entry);
                }
            }
            Entry fresh;
            fresh.key = data.uid;
            fresh.scaleHash = scaleHash;
            fresh.generation = 1;
            if (!Build(device, data, paletteTileScales, fresh))
            {
                return PaletteGpu{};
            }
            m_entries.PushBack(fresh);
            return MakeGpu(m_entries[m_entries.Size() - 1]);
        }

        /// Drops every entry. This runs at scene destroy as well as at shutdown (stopping
        /// play-in-editor destroys the run's scenes mid frame loop), so a live entry may still
        /// sit in a submitted frame's descriptor set: with a retire queue wired the GPU objects
        /// age past every in-flight frame before they are freed (VUID 01026); without one
        /// (Null-device tests) they are destroyed in place.
        void Clear(rhi::Device& device)
        {
            for (Entry& entry : m_entries)
            {
                RetireOrDestroy(device, entry);
            }
            m_entries.Clear();
        }

        [[nodiscard]] usize Size() const noexcept { return m_entries.Size(); }

    private:
        struct Entry
        {
            u64 key = 0; // TerrainPaletteData::uid, never a pointer
            u64 scaleHash = 0;
            u64 generation = 0;
            rhi::Texture* arrayTexture = nullptr;       // albedo (sRGB)
            rhi::TextureView* arrayView = nullptr;
            rhi::Texture* normalTexture = nullptr;      // linear; null when no layer used a normal map
            rhi::TextureView* normalArrayView = nullptr;
            rhi::Texture* ormTexture = nullptr;         // linear; null when no layer used an ORM map
            rhi::TextureView* ormArrayView = nullptr;
            rhi::Texture* heightTexture = nullptr;      // linear; null when no layer used a height map
            rhi::TextureView* heightArrayView = nullptr;
            rhi::Texture* maskTexture = nullptr;        // linear; null when no layer used a mask
            rhi::TextureView* maskArrayView = nullptr;
            rhi::Buffer* tileScaleBuffer = nullptr;
        };

        [[nodiscard]] static PaletteGpu MakeGpu(const Entry& e) noexcept
        {
            return PaletteGpu{e.arrayView,       e.normalArrayView, e.ormArrayView, e.heightArrayView,
                              e.maskArrayView,   e.tileScaleBuffer, e.generation};
        }

        [[nodiscard]] static u64 HashScales(Span<const f32> scales) noexcept
        {
            u64 h = kFnv1a64OffsetBasis; // FNV-1a
            const auto mix = [&h](f32 v)
            {
                u32 bits;
                MemCopy(&bits, &v, sizeof(bits));
                h = (h ^ bits) * kFnv1a64Prime;
            };
            for (usize i = 0; i < scales.Size(); ++i)
            {
                mix(scales[i]);
            }
            return h;
        }

        // Creates and uploads one RGBA8 Texture2DArray (slice-major mip chain) at `format`.
        [[nodiscard]] static bool BuildArray(rhi::Device& device, rhi::TextureFormat format,
                                             Span<const u8> texels, u32 sliceSize, u32 mipCount,
                                             u32 sliceCount, StringView label, rhi::Texture*& outTex,
                                             rhi::TextureView*& outView)
        {
            rhi::TextureDesc td{};
            td.format = format;
            td.width = sliceSize;
            td.height = sliceSize;
            td.arrayLayerCount = sliceCount;
            td.mipLevelCount = mipCount;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
            td.label = label;
            if (!device.CreateTexture(td, outTex).IsOk() || outTex == nullptr)
            {
                return false;
            }
            rhi::TextureViewDesc vd{};
            vd.format = format;
            vd.dimension = rhi::TextureViewDimension::Texture2DArray;
            vd.arrayLayerCount = sliceCount;
            vd.mipLevelCount = mipCount;
            if (!device.CreateTextureView(outTex, vd, outView).IsOk() || outView == nullptr)
            {
                device.DestroyTexture(outTex);
                outTex = nullptr;
                return false;
            }
            if (rhi::Queue* queue = device.GetQueue(rhi::QueueType::Graphics))
            {
                rhi::TransferBatch* batch = nullptr;
                if (queue->CreateTransferBatch(batch).IsOk() && batch != nullptr)
                {
                    const usize sliceBytes =
                        foundation::terrain::TerrainPaletteData::SliceBytes(sliceSize, mipCount);
                    for (u32 slice = 0; slice < sliceCount; ++slice)
                    {
                        const u8* sliceBase = texels.Data() + sliceBytes * slice;
                        usize offset = 0;
                        u32 dim = sliceSize;
                        for (u32 m = 0; m < mipCount; ++m)
                        {
                            const usize bytes = static_cast<usize>(dim) * dim * 4u;
                            rhi::TextureDataLayout layout{};
                            layout.bytesPerRow = dim * 4u;
                            layout.rowsPerImage = dim;
                            batch->WriteTexture(outTex, Span<const u8>{sliceBase + offset, bytes},
                                                layout, rhi::Extent3D{dim, dim, 1}, m, slice);
                            offset += bytes;
                            dim = dim > 1 ? dim / 2 : 1;
                        }
                    }
                    (void)batch->Submit();
                    queue->DestroyTransferBatch(batch);
                }
            }
            return true;
        }

        [[nodiscard]] static bool Build(rhi::Device& device,
                                        const foundation::terrain::TerrainPaletteData& data,
                                        Span<const f32> paletteTileScales, Entry& out)
        {
            // Albedo is sRGB, so the palette matches the base's brightness; normal and ORM are
            // linear and built only when a layer supplied them (otherwise the renderer binds a 1x1
            // dummy).
            if (!BuildArray(device, rhi::TextureFormat::RGBA8UnormSrgb,
                            Span<const u8>{data.texels.Data(), data.texels.Size()}, data.sliceSize,
                            data.mipCount, data.sliceCount, u8"terrain.palette", out.arrayTexture,
                            out.arrayView))
            {
                return false;
            }
            if (data.HasNormal() &&
                !BuildArray(device, rhi::TextureFormat::RGBA8Unorm,
                            Span<const u8>{data.normalTexels.Data(), data.normalTexels.Size()},
                            data.sliceSize, data.mipCount, data.sliceCount, u8"terrain.palette.normal",
                            out.normalTexture, out.normalArrayView))
            {
                Destroy(device, out);
                return false;
            }
            if (data.HasOrm() &&
                !BuildArray(device, rhi::TextureFormat::RGBA8Unorm,
                            Span<const u8>{data.ormTexels.Data(), data.ormTexels.Size()},
                            data.sliceSize, data.mipCount, data.sliceCount, u8"terrain.palette.orm",
                            out.ormTexture, out.ormArrayView))
            {
                Destroy(device, out);
                return false;
            }
            if (data.HasHeight() &&
                !BuildArray(device, rhi::TextureFormat::RGBA8Unorm,
                            Span<const u8>{data.heightTexels.Data(), data.heightTexels.Size()},
                            data.sliceSize, data.mipCount, data.sliceCount, u8"terrain.palette.height",
                            out.heightTexture, out.heightArrayView))
            {
                Destroy(device, out);
                return false;
            }
            if (data.HasMask() &&
                !BuildArray(device, rhi::TextureFormat::RGBA8Unorm,
                            Span<const u8>{data.maskTexels.Data(), data.maskTexels.Size()},
                            data.sliceSize, data.mipCount, data.sliceCount, u8"terrain.palette.mask",
                            out.maskTexture, out.maskArrayView))
            {
                Destroy(device, out);
                return false;
            }

            // The tileScale storage buffer: TileScales[i] = palette layer i (tight f32; the base
            // tile rides the view UBO, so a base-tile edit never rebuilds this buffer).
            Array<f32> scales;
            for (u32 i = 0; i < data.sliceCount; ++i)
            {
                scales.PushBack(i < paletteTileScales.Size() ? paletteTileScales[i] : 1.0f);
            }
            rhi::BufferDesc bd{};
            bd.size = scales.Size() * sizeof(f32);
            // StorageRead, not Storage: the shader only reads it, and CpuToGpu (upload-heap) memory
            // cannot carry UAV/Storage usage (the DX12 UPLOAD heap rule, which RHI validation checks).
            bd.usage = rhi::BufferUsage::StorageRead | rhi::BufferUsage::CopyDst;
            bd.memory = rhi::MemoryLocation::CpuToGpu;
            bd.label = u8"terrain.tileScales";
            if (!device.CreateBuffer(bd, out.tileScaleBuffer).IsOk() ||
                out.tileScaleBuffer == nullptr)
            {
                Destroy(device, out);
                return false;
            }
            if (void* mapped = out.tileScaleBuffer->Map())
            {
                MemCopy(mapped, scales.Data(), scales.Size() * sizeof(f32));
                out.tileScaleBuffer->Unmap();
            }
            return true;
        }

        void RetireOrDestroy(rhi::Device& device, Entry& entry)
        {
            if (m_retire != nullptr)
            {
                m_retire->Retire(entry.arrayView);
                m_retire->Retire(entry.arrayTexture);
                m_retire->Retire(entry.normalArrayView);
                m_retire->Retire(entry.normalTexture);
                m_retire->Retire(entry.ormArrayView);
                m_retire->Retire(entry.ormTexture);
                m_retire->Retire(entry.heightArrayView);
                m_retire->Retire(entry.heightTexture);
                m_retire->Retire(entry.maskArrayView);
                m_retire->Retire(entry.maskTexture);
                m_retire->Retire(entry.tileScaleBuffer);
                entry.arrayView = nullptr;
                entry.arrayTexture = nullptr;
                entry.normalArrayView = nullptr;
                entry.normalTexture = nullptr;
                entry.ormArrayView = nullptr;
                entry.ormTexture = nullptr;
                entry.heightArrayView = nullptr;
                entry.heightTexture = nullptr;
                entry.maskArrayView = nullptr;
                entry.maskTexture = nullptr;
                entry.tileScaleBuffer = nullptr;
                return;
            }
            Destroy(device, entry);
        }

        static void Destroy(rhi::Device& device, Entry& entry)
        {
            const auto view = [&device](rhi::TextureView*& v)
            {
                if (v != nullptr)
                {
                    device.DestroyTextureView(v);
                    v = nullptr;
                }
            };
            const auto tex = [&device](rhi::Texture*& t)
            {
                if (t != nullptr)
                {
                    device.DestroyTexture(t);
                    t = nullptr;
                }
            };
            view(entry.arrayView);
            tex(entry.arrayTexture);
            view(entry.normalArrayView);
            tex(entry.normalTexture);
            view(entry.ormArrayView);
            tex(entry.ormTexture);
            view(entry.heightArrayView);
            tex(entry.heightTexture);
            view(entry.maskArrayView);
            tex(entry.maskTexture);
            if (entry.tileScaleBuffer != nullptr)
            {
                device.DestroyBuffer(entry.tileScaleBuffer);
                entry.tileScaleBuffer = nullptr;
            }
        }

        Array<Entry> m_entries;
        render::GpuRetireQueue* m_retire = nullptr; // borrowed (RenderSubsystem owns and ticks it)
    };
}
