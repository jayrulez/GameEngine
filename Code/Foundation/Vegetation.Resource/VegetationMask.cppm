// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// foundation.vegetation.resource:mask: the painted vegetation mask.
//
// `planeCount` u8 density planes (0: nothing grows, 255: the layer's full density) over the
// terrain's 0..1 footprint UV, plane-major in one blob. A vegetation layer with Mask placement
// names its plane; the scatter reads the density at each candidate. It is an Object so it is a
// referenceable product (Ref<VegetationMask>) the vegetation component holds; the per-object
// uid and Version() drive the scatter cache invalidation, as SplatWeights' do.
//
// Cooked form, as for the splat: the metadata object `VegetationMaskSource` (dims and plane
// count) and one sidecar stream `kVegetationMaskStream` ("densities") carrying every plane.
// `VegetationMaskFactory` builds the runtime mask from a cooked instance.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.vegetation.resource:mask;

import foundation.core;
import foundation.resource;
import foundation.content;

using namespace foundation::core;
using namespace foundation::resource;

export namespace foundation::vegetation
{
    /// The largest plane count a mask carries (a PNG import yields one plane per channel).
    inline constexpr u32 kMaxMaskPlanes = 16;

    class VegetationMask final : public Object
    {
        RTTI_OBJECT(VegetationMask, Object)
    public:
        // Unique per-object id: caches key by this and Version(), never by pointer, since a
        // freed mask's address can be reused by another.
        const u64 uid = NextUid();
        [[nodiscard]] static u64 NextUid() noexcept
        {
            static Atomic<u64> counter{0};
            return counter.fetch_add(1) + 1;
        }

        VegetationMask() = default;

        /// Allocate `planeCount` all-zero planes: nothing grows until painted.
        VegetationMask(i32 width, i32 height, u32 planeCount)
            : m_width(width), m_height(height),
              m_planeCount(planeCount == 0 ? 1u : (planeCount > kMaxMaskPlanes ? kMaxMaskPlanes
                                                                               : planeCount))
        {
            if (width > 0 && height > 0)
            {
                m_densities.Resize(PlaneBytes() * m_planeCount, u8{0});
            }
        }

        [[nodiscard]] bool IsEmpty() const noexcept { return m_width <= 0 || m_height <= 0; }
        [[nodiscard]] i32 Width() const noexcept { return m_width; }
        [[nodiscard]] i32 Height() const noexcept { return m_height; }
        [[nodiscard]] u32 PlaneCount() const noexcept { return m_planeCount; }
        [[nodiscard]] usize PlaneBytes() const noexcept
        {
            return static_cast<usize>(m_width) * static_cast<usize>(m_height);
        }

        /// Bumped by every brush op and by a load; the scatter cache regrows on a change.
        [[nodiscard]] u64 Version() const noexcept { return m_version; }
        void BumpVersion() noexcept { ++m_version; }

        [[nodiscard]] Span<const u8> Densities() const noexcept
        {
            return {m_densities.Data(), m_densities.Size()};
        }
        [[nodiscard]] Span<u8> Densities() noexcept { return {m_densities.Data(), m_densities.Size()}; }

        /// One plane's raster (row-major, `Width() x Height()`); empty for a plane out of range.
        [[nodiscard]] Span<const u8> Plane(u32 plane) const noexcept
        {
            if (plane >= m_planeCount || IsEmpty())
            {
                return {};
            }
            return {m_densities.Data() + PlaneBytes() * plane, PlaneBytes()};
        }
        [[nodiscard]] Span<u8> Plane(u32 plane) noexcept
        {
            if (plane >= m_planeCount || IsEmpty())
            {
                return {};
            }
            return {m_densities.Data() + PlaneBytes() * plane, PlaneBytes()};
        }

        /// The density (0..255) of `plane` at texel (x, y); 0 outside the raster or the planes.
        [[nodiscard]] u8 DensityAt(u32 plane, i32 x, i32 y) const noexcept
        {
            if (plane >= m_planeCount || x < 0 || y < 0 || x >= m_width || y >= m_height)
            {
                return 0;
            }
            return m_densities[PlaneBytes() * plane + static_cast<usize>(y) * static_cast<usize>(m_width) +
                               static_cast<usize>(x)];
        }
        void SetDensity(u32 plane, i32 x, i32 y, u8 value) noexcept
        {
            if (plane >= m_planeCount || x < 0 || y < 0 || x >= m_width || y >= m_height)
            {
                return;
            }
            m_densities[PlaneBytes() * plane + static_cast<usize>(y) * static_cast<usize>(m_width) +
                        static_cast<usize>(x)] = value;
        }

        /// The density share (0..1) of `plane` at a 0..1 footprint UV (nearest texel, clamped).
        [[nodiscard]] f32 ShareAt(u32 plane, f32 u, f32 v) const noexcept
        {
            if (plane >= m_planeCount || IsEmpty())
            {
                return 0.0f;
            }
            const f32 cu = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
            const f32 cv = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            const i32 x = static_cast<i32>(cu * static_cast<f32>(m_width - 1) + 0.5f);
            const i32 y = static_cast<i32>(cv * static_cast<f32>(m_height - 1) + 0.5f);
            return static_cast<f32>(DensityAt(plane, x, y)) / 255.0f;
        }

    private:
        i32 m_width = 0;
        i32 m_height = 0;
        u32 m_planeCount = 1;
        u64 m_version = 1;
        Array<u8> m_densities;
    };

    /// The inclusive texel rectangle a brush op touched (region-delta undo and the chunk regrow).
    struct MaskRegion
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
        // The elliptical brush visitor (the splat brush's shape): calls fn(x, y, t) for every
        // texel inside the world circle (per-axis UV radii) with the falloff-scaled strength t in
        // (0, 1]; `coreFraction` is the flat inner core's share of the radius (full strength
        // inside it, a cosine skirt outside).
        template <typename TFn>
        inline void VisitMaskBrush(const VegetationMask& mask, f32 uvX, f32 uvY, f32 uvRadiusX,
                                   f32 uvRadiusY, f32 amount, f32 coreFraction, TFn&& fn)
        {
            const i32 w = mask.Width();
            const i32 h = mask.Height();
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
            const f32 core = Clamp(coreFraction, 0.0f, 0.95f);
            for (i32 y = y0; y <= y1; ++y)
            {
                for (i32 x = x0; x <= x1; ++x)
                {
                    const f32 du =
                        ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(w) - uvX) * invRx;
                    const f32 dv =
                        ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(h) - uvY) * invRy;
                    const f32 dist = Sqrt(du * du + dv * dv); // 0 centre .. 1 rim
                    if (dist >= 1.0f)
                    {
                        continue;
                    }
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

    /// Paint `plane` over a world-space brush disc: d = d + t * (255 - d) per texel (repeated
    /// full-strength painting converges to 255). Bumps the version when anything changed;
    /// returns the touched rect (empty for a plane out of range or a no-op).
    inline MaskRegion PaintMask(VegetationMask& mask, u32 plane, f32 uvX, f32 uvY, f32 uvRadiusX,
                                f32 uvRadiusY, f32 amount, f32 coreFraction = 0.5f)
    {
        MaskRegion region;
        if (mask.IsEmpty() || plane >= mask.PlaneCount() || uvRadiusX <= 0.0f ||
            uvRadiusY <= 0.0f || amount <= 0.0f)
        {
            return region;
        }
        Span<u8> px = mask.Plane(plane);
        const i32 w = mask.Width();
        bool changed = false;
        detail::VisitMaskBrush(mask, uvX, uvY, uvRadiusX, uvRadiusY, amount, coreFraction,
                               [&](i32 x, i32 y, f32 t)
                               {
                                   u8& d = px[static_cast<usize>(y) * static_cast<usize>(w) +
                                              static_cast<usize>(x)];
                                   const f32 raised = static_cast<f32>(d) +
                                                      t * (255.0f - static_cast<f32>(d));
                                   const u8 q = static_cast<u8>(Min(255.0f, raised + 0.5f));
                                   if (q != d)
                                   {
                                       d = q;
                                       changed = true;
                                       region.Add(x, y);
                                   }
                               });
        if (changed)
        {
            mask.BumpVersion();
        }
        return region;
    }

    /// Erase `plane` over the disc: d *= (1 - t) per texel (repeated full-strength erasing
    /// reaches 0).
    inline MaskRegion EraseMask(VegetationMask& mask, u32 plane, f32 uvX, f32 uvY, f32 uvRadiusX,
                                f32 uvRadiusY, f32 amount, f32 coreFraction = 0.5f)
    {
        MaskRegion region;
        if (mask.IsEmpty() || plane >= mask.PlaneCount() || uvRadiusX <= 0.0f ||
            uvRadiusY <= 0.0f || amount <= 0.0f)
        {
            return region;
        }
        Span<u8> px = mask.Plane(plane);
        const i32 w = mask.Width();
        bool changed = false;
        detail::VisitMaskBrush(mask, uvX, uvY, uvRadiusX, uvRadiusY, amount, coreFraction,
                               [&](i32 x, i32 y, f32 t)
                               {
                                   u8& d = px[static_cast<usize>(y) * static_cast<usize>(w) +
                                              static_cast<usize>(x)];
                                   const f32 faded = static_cast<f32>(d) * (1.0f - t);
                                   // A full-strength erase lands on exactly 0 (round-to-nearest
                                   // would otherwise leave a 1 behind at tiny t remainders).
                                   const u8 q = static_cast<u8>(faded + 0.5f);
                                   if (q != d)
                                   {
                                       d = q;
                                       changed = true;
                                       region.Add(x, y);
                                   }
                               });
        if (changed)
        {
            mask.BumpVersion();
        }
        return region;
    }

    /// Smooth `plane` over the disc: each texel moves toward its 3x3 neighbourhood mean by t
    /// (computed from a snapshot of the disc, so the blur never feeds on itself in one stamp).
    inline MaskRegion SmoothMask(VegetationMask& mask, u32 plane, f32 uvX, f32 uvY, f32 uvRadiusX,
                                 f32 uvRadiusY, f32 amount, f32 coreFraction = 0.5f)
    {
        MaskRegion region;
        if (mask.IsEmpty() || plane >= mask.PlaneCount() || uvRadiusX <= 0.0f ||
            uvRadiusY <= 0.0f || amount <= 0.0f)
        {
            return region;
        }
        const i32 w = mask.Width();
        const i32 h = mask.Height();
        Array<u8> before;
        before.Resize(mask.PlaneBytes());
        MemCopy(before.Data(), mask.Plane(plane).Data(), mask.PlaneBytes());
        Span<u8> px = mask.Plane(plane);
        bool changed = false;
        detail::VisitMaskBrush(
            mask, uvX, uvY, uvRadiusX, uvRadiusY, amount, coreFraction,
            [&](i32 x, i32 y, f32 t)
            {
                i32 sum = 0;
                i32 n = 0;
                for (i32 dy = -1; dy <= 1; ++dy)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        const i32 sx = x + dx;
                        const i32 sy = y + dy;
                        if (sx < 0 || sy < 0 || sx >= w || sy >= h)
                        {
                            continue;
                        }
                        sum += before[static_cast<usize>(sy) * static_cast<usize>(w) +
                                      static_cast<usize>(sx)];
                        ++n;
                    }
                }
                const f32 mean = n > 0 ? static_cast<f32>(sum) / static_cast<f32>(n) : 0.0f;
                u8& d = px[static_cast<usize>(y) * static_cast<usize>(w) + static_cast<usize>(x)];
                const f32 moved = static_cast<f32>(d) + t * (mean - static_cast<f32>(d));
                const u8 q = static_cast<u8>(Clamp(moved + 0.5f, 0.0f, 255.0f));
                if (q != d)
                {
                    d = q;
                    changed = true;
                    region.Add(x, y);
                }
            });
        if (changed)
        {
            mask.BumpVersion();
        }
        return region;
    }

    /// Cooked mask metadata: dims and plane count. The planes travel in the
    /// `kVegetationMaskStream` sidecar, keeping bulk data out of the serialized object.
    class VegetationMaskSource final : public ISerializable
    {
        RTTI_OBJECT(VegetationMaskSource, ISerializable)
    public:
        i32 width = 0;
        i32 height = 0;
        u32 planeCount = 1;

        void Serialize(ISerializer& ar) override
        {
            foundation::core::Serialize(ar, "width", width);
            foundation::core::Serialize(ar, "height", height);
            foundation::core::Serialize(ar, "planeCount", planeCount);
        }

        static void FromMask(const VegetationMask& mask, VegetationMaskSource& out)
        {
            out.width = mask.Width();
            out.height = mask.Height();
            out.planeCount = mask.PlaneCount();
        }

        [[nodiscard]] static Span<const byte> DensityBlob(const VegetationMask& mask) noexcept
        {
            const Span<const u8> px = mask.Densities();
            return Span<const byte>(reinterpret_cast<const byte*>(px.Data()), px.Size());
        }

        /// Build the runtime product from this metadata and the sidecar blob. Inconsistent data
        /// (bad dims, a blob size mismatch) yields an empty mask rather than a malformed one.
        [[nodiscard]] RefPtr<VegetationMask> Build(Span<const byte> densityBlob,
                                                   IAllocator& allocator) const
        {
            if (width <= 0 || height <= 0 || planeCount == 0 || planeCount > kMaxMaskPlanes)
            {
                return MakeRef<VegetationMask>(allocator);
            }
            const usize expected = static_cast<usize>(width) * static_cast<usize>(height) *
                                   static_cast<usize>(planeCount);
            if (densityBlob.Size() != expected)
            {
                return MakeRef<VegetationMask>(allocator);
            }
            RefPtr<VegetationMask> mask = MakeRef<VegetationMask>(allocator, width, height, planeCount);
            MemCopy(mask->Densities().Data(), densityBlob.Data(), expected);
            return mask;
        }
    };

    /// The sidecar stream carrying every plane, plane-major.
    inline constexpr StringView kVegetationMaskStream = u8"densities";

    /// Builds a cooked mask (metadata and the stream) into a runtime VegetationMask. Pure-CPU,
    /// async-safe.
    class VegetationMaskFactory final : public IResourceFactory
    {
    public:
        explicit VegetationMaskFactory(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        [[nodiscard]] const TypeInfo* ProductType() const override
        {
            return &VegetationMask::StaticType();
        }
        [[nodiscard]] const TypeInfo* CookedType() const override
        {
            return &VegetationMaskSource::StaticType();
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
        [[nodiscard]] RefPtr<Object> BuildFrom(foundation::content::Instance& instance) const
        {
            RefPtr<ISerializable> object = instance.ReadObject();
            VegetationMaskSource* src = Cast<VegetationMaskSource>(object.Get());
            if (src == nullptr)
            {
                return RefPtr<Object>{};
            }
            Array<u8> blob;
            if (UniquePtr<IStream> s = instance.ReadData(kVegetationMaskStream))
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
            return src->Build(
                Span<const byte>{reinterpret_cast<const byte*>(blob.Data()), blob.Size()},
                *m_allocator);
        }

        IAllocator* m_allocator;
    };

    /// Register the mask resource types (product + cooked source) for load.
    inline void RegisterVegetationMaskResourceTypes()
    {
        GlobalTypeRegistry().Register(VegetationMask::StaticType());
        GlobalTypeRegistry().Register(VegetationMaskSource::StaticType());
        RegisterSerializable<VegetationMaskSource>();
    }

    RTTI_DEFINE_OBJECT(VegetationMask, "rtti::vegetation")
    RTTI_DEFINE_OBJECT_VERSIONED(VegetationMaskSource, "rtti::vegetation", 1)
}

export namespace foundation::vegetation
{
    /// The vegetation resource module: the engine composition composes this library's
    /// factories from it.
    inline constexpr foundation::resource::ResourceFactoryDesc kVegetationResourceFactories[] = {
        foundation::resource::FactoryWithAllocator<VegetationMask, VegetationMaskSource, VegetationMaskFactory>(),
    };
    inline constexpr foundation::resource::ResourceModule kVegetationResourceModule{
        u8"vegetation", &RegisterVegetationMaskResourceTypes, kVegetationResourceFactories,
        sizeof(kVegetationResourceFactories) / sizeof(kVegetationResourceFactories[0])};
}
