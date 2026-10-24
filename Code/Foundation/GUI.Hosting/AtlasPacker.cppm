// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Hosting - :atlas_packer partition: ShelfPacker, where the picture baker's atlas puts each
// picture on a page. Rows (shelves) across the page, each as tall as the first picture it took;
// a picture goes on the lowest shelf it fits that wastes the least height (at most a third more
// than it needs), else on a new shelf under the last. A page only fills: what goes leaves its
// place empty until the atlas packs its page anew (Reset), as a font atlas does. Pure arithmetic,
// no device.

module;
#include "Core/Prelude.h"

export module foundation.gui.hosting:atlas_packer;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    /// A place on a page: its top-left corner in pixels.
    struct PackedPlace
    {
        u32 X = 0;
        u32 Y = 0;
    };

    class ShelfPacker
    {
    public:
        ShelfPacker(IAllocator& allocator, u32 width, u32 height) noexcept
            : m_width(width), m_height(height), m_shelves(allocator)
        {
        }

        [[nodiscard]] u32 Width() const noexcept { return m_width; }
        [[nodiscard]] u32 Height() const noexcept { return m_height; }

        /// A place for `width` by `height` pixels (the caller's gutter included): its top-left
        /// corner, or nothing when the page has no room left for it.
        [[nodiscard]] Optional<PackedPlace> Place(u32 width, u32 height)
        {
            if (width == 0 || height == 0 || width > m_width || height > m_height)
                return {};
            Shelf* best = nullptr;
            for (Shelf& shelf : m_shelves)
                if (shelf.Height >= height && shelf.Height * 3 <= height * 4 && shelf.Used + width <= m_width &&
                    (best == nullptr || shelf.Height < best->Height))
                    best = &shelf;
            if (best == nullptr)
            {
                if (m_top + height > m_height)
                    return {};
                m_shelves.PushBack(Shelf{m_top, height, 0});
                m_top += height;
                best = &m_shelves.Back();
            }
            const PackedPlace at{best->Used, best->Top};
            best->Used += width;
            m_area += static_cast<u64>(width) * height;
            return at;
        }

        /// Empties the page: every place free again.
        void Reset() noexcept
        {
            m_shelves.Clear();
            m_top = 0;
            m_area = 0;
        }

        /// The pixels placed since the page was last emptied.
        [[nodiscard]] u64 AreaUsed() const noexcept { return m_area; }

    private:
        struct Shelf
        {
            u32 Top = 0;
            u32 Height = 0;
            u32 Used = 0; // across, from the left
        };

        u32 m_width = 0;
        u32 m_height = 0;
        Array<Shelf> m_shelves;
        u32 m_top = 0; // under the last shelf
        u64 m_area = 0;
    };
}
