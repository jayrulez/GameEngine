// Draconic::Image - :atlas_builder partition.
//
// RectI (integer rectangle for atlas regions) and ImageAtlasBuilder - a
// general-purpose shelf-packing atlas packer that combines multiple RGBA8
// images into one atlas texture (UI themes, sprite sheets, ...). Ported from
// Sedulous.Images/ImageAtlasBuilder.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.image:atlas_builder;

import draconic.foundation;
import :pixel_format;
import :image_data;

using namespace draconic::foundation;

export namespace draconic::image
{
    /// Integer rectangle for atlas regions.
    struct RectI
    {
        i32 x = 0;
        i32 y = 0;
        i32 width = 0;
        i32 height = 0;

        constexpr RectI() noexcept = default;
        constexpr RectI(i32 inX, i32 inY, i32 w, i32 h) noexcept
            : x(inX), y(inY), width(w), height(h)
        {
        }
    };

    /// General-purpose image atlas packer (shelf packing). Combines RGBA8 images
    /// into one atlas texture. Images are not owned (caller keeps them alive).
    class ImageAtlasBuilder
    {
    public:
        /// minSize/maxSize: atlas dimensions (rounded up to powers of two).
        /// padding: pixels between packed images.
        explicit ImageAtlasBuilder(u32 minSize = 256, u32 maxSize = 4096, u32 padding = 1)
            : m_minSize(NextPowerOf2(minSize)), m_maxSize(NextPowerOf2(maxSize)), m_padding(padding)
        {
        }

        /// The built atlas image. Null until Build() succeeds.
        [[nodiscard]] const ImageData* Atlas() const { return m_built ? &m_atlas : nullptr; }

        /// Number of entries added.
        [[nodiscard]] usize EntryCount() const { return m_entries.Size(); }

        /// Add an image to be packed. Name must be unique. Image is not owned.
        void AddImage(StringView name, const ImageData* image)
        {
            if (image == nullptr)
                return;
            Entry entry;
            entry.name = String(name);
            entry.image = image;
            m_entries.PushBack(Move(entry));
        }

        /// Pack all added images into a single RGBA8 atlas. Returns true on success.
        bool Build()
        {
            if (m_entries.IsEmpty())
            {
                const u8 emptyPixel[4] = {0, 0, 0, 0};
                m_atlas = OwnedImageData(1, 1, PixelFormat::RGBA8, Span<const u8>(emptyPixel, 4));
                m_built = true;
                return true;
            }

            // Sort by height descending for better shelf packing.
            SortByHeightDesc();

            // Try increasing atlas sizes until everything fits.
            for (u32 size = m_minSize; size <= m_maxSize; size *= 2)
            {
                if (TryPack(size, size))
                {
                    m_built = true;
                    return true;
                }
            }
            return false; // Couldn't fit in max size.
        }

        /// The pixel-space region of a packed image by name (null if not found).
        [[nodiscard]] const RectI* GetRegion(StringView name) const
        {
            return m_regions.Find(String(name));
        }

    private:
        struct Entry
        {
            String name;
            const ImageData* image = nullptr;
        };

        void SortByHeightDesc()
        {
            // Insertion sort (entry counts are small); stable-ish, descending height.
            for (usize i = 1; i < m_entries.Size(); ++i)
            {
                Entry key = Move(m_entries[i]);
                const u32 keyH = key.image->Height();
                usize j = i;
                while (j > 0 && m_entries[j - 1].image->Height() < keyH)
                {
                    m_entries[j] = Move(m_entries[j - 1]);
                    --j;
                }
                m_entries[j] = Move(key);
            }
        }

        bool TryPack(u32 atlasW, u32 atlasH)
        {
            m_regions.Clear();

            // Shelf packing: place images left-to-right, start a new row when full.
            u32 curX = m_padding;
            u32 curY = m_padding;
            u32 rowHeight = 0;

            for (usize e = 0; e < m_entries.Size(); ++e)
            {
                const Entry& entry = m_entries[e];
                const u32 imgW = entry.image->Width();
                const u32 imgH = entry.image->Height();

                if (curX + imgW + m_padding > atlasW)
                {
                    curX = m_padding;
                    curY += rowHeight + m_padding;
                    rowHeight = 0;
                }

                if (curY + imgH + m_padding > atlasH)
                    return false;

                m_regions.InsertOrAssign(String(entry.name.AsView()),
                                         RectI{static_cast<i32>(curX), static_cast<i32>(curY),
                                               static_cast<i32>(imgW), static_cast<i32>(imgH)});

                curX += imgW + m_padding;
                rowHeight = Max(rowHeight, imgH);
            }

            // Build the atlas pixel data.
            Array<u8> pixelData;
            pixelData.Resize(static_cast<usize>(atlasW) * atlasH * 4);
            MemSet(pixelData.Data(), 0, pixelData.Size());

            for (usize e = 0; e < m_entries.Size(); ++e)
            {
                const Entry& entry = m_entries[e];
                const RectI* region = m_regions.Find(String(entry.name.AsView()));
                if (region == nullptr)
                    continue;

                const ImageData* src = entry.image;
                if (src->Format() == PixelFormat::RGBA8)
                {
                    const Span<const u8> srcData = src->PixelData();
                    const u32 srcStride = src->Width() * 4;
                    const u32 dstStride = atlasW * 4;

                    for (u32 y = 0; y < src->Height(); ++y)
                    {
                        const u32 srcOffset = y * srcStride;
                        const u32 dstOffset = (static_cast<u32>(region->y) + y) * dstStride +
                                              static_cast<u32>(region->x) * 4;

                        if (srcOffset + srcStride <= srcData.Size() &&
                            dstOffset + srcStride <= pixelData.Size())
                            MemCopy(pixelData.Data() + dstOffset, srcData.Data() + srcOffset,
                                    srcStride);
                    }
                }
            }

            m_atlas = OwnedImageData(atlasW, atlasH, PixelFormat::RGBA8, Move(pixelData));
            return true;
        }

        [[nodiscard]] static u32 NextPowerOf2(u32 v)
        {
            u32 n = v;
            --n;
            n |= n >> 1;
            n |= n >> 2;
            n |= n >> 4;
            n |= n >> 8;
            n |= n >> 16;
            ++n;
            return Max(n, 1u);
        }

        Array<Entry> m_entries;
        OwnedImageData m_atlas;
        HashMap<String, RectI> m_regions;
        u32 m_minSize;
        u32 m_maxSize;
        u32 m_padding;
        bool m_built = false;
    };
}
