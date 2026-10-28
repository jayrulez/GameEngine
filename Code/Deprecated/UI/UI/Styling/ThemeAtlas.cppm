// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :theme_atlas partition
//
// Builds a packed image atlas for theme drawables. Wraps ImageAtlasBuilder and creates
// AtlasImageDrawable/AtlasNineSliceDrawable from packed regions. Single atlas texture = zero
// texture switches during UI rendering. Ported from Sedulous.UI/src/Styling/ThemeAtlas.bf.
//
// Divergences (language): the Beef `ImageAtlasBuilder mBuilder ~ delete _` owned pointer becomes a
// by-value member; factories return RefPtr<...> (drawables are Object/RefPtr here, not raw owning
// pointers); CreateStateDrawable's Beef tuple span `Span<(ControlState, StringView)>` becomes a
// Span<const StateImageEntry>.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:theme_atlas;

import foundation.core;  // Color, RefPtr, Span, StringView, Rectangle
import foundation.image; // ImageAtlasBuilder, ImageData, NineSlice, RectI
import :thickness;
import :control_state;
import :drawable;
import :atlas_image_drawable;
import :atlas_nine_slice_drawable;
import :state_list_drawable;

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
{
    /// A (ControlState, region-name) pair for building a StateListDrawable from atlas regions.
    struct StateImageEntry
    {
        ControlState State = ControlState::Normal;
        StringView Name{};
    };

    /// Builds a packed image atlas for theme drawables and creates atlas-backed drawables from it.
    /// Derives Object so a StyleSheet can hold it via OwnResource (RefPtr<Object>) - the textured theme
    /// keeps the atlas alive as long as the atlas-backed drawables that reference it. Not scripted.
    class ThemeAtlas : public Object
    {
        RTTI_OBJECT(ThemeAtlas, Object)
    public:
        explicit ThemeAtlas(u32 minSize = 256, u32 maxSize = 4096, u32 padding = 1)
            : m_builder(minSize, maxSize, padding)
        {
        }

        /// The built atlas image (null until Build() succeeds).
        [[nodiscard]] const image::ImageData* Atlas() const { return m_builder.Atlas(); }

        /// Add an image to be packed into the atlas.
        void AddImage(StringView name, const image::ImageData* image)
        {
            m_builder.AddImage(name, image);
        }

        /// Pack all added images. Must be called before creating drawables.
        bool Build()
        {
            m_built = m_builder.Build();
            return m_built;
        }

        /// Create an AtlasImageDrawable for a named region.
        [[nodiscard]] RefPtr<AtlasImageDrawable> CreateImageDrawable(StringView name,
                                                                     Color tint = Color::White)
        {
            if (!m_built || m_builder.Atlas() == nullptr)
            {
                return nullptr;
            }
            const image::RectI* region = m_builder.GetRegion(name);
            if (region == nullptr)
            {
                return nullptr;
            }

            const image::RectI& r = *region;
            return MakeRef<AtlasImageDrawable>(
                MemoryAllocator(), m_builder.Atlas(),
                Rectangle{static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.width),
                          static_cast<f32>(r.height)},
                tint);
        }

        /// Create an AtlasNineSliceDrawable for a named region.
        [[nodiscard]] RefPtr<AtlasNineSliceDrawable>
        CreateNineSliceDrawable(StringView name, image::NineSlice slices, Color tint = Color::White,
                                Thickness expand = {})
        {
            if (!m_built || m_builder.Atlas() == nullptr)
            {
                return nullptr;
            }
            const image::RectI* region = m_builder.GetRegion(name);
            if (region == nullptr)
            {
                return nullptr;
            }

            const image::RectI& r = *region;
            return MakeRef<AtlasNineSliceDrawable>(
                MemoryAllocator(), m_builder.Atlas(),
                Rectangle{static_cast<f32>(r.x), static_cast<f32>(r.y), static_cast<f32>(r.width),
                          static_cast<f32>(r.height)},
                slices, tint, expand);
        }

        /// Create a StateListDrawable with atlas-backed entries for multiple states.
        [[nodiscard]] RefPtr<StateListDrawable>
        CreateStateDrawable(Span<const StateImageEntry> stateImages, image::NineSlice slices = {},
                            Color tint = Color::White, Thickness expand = {})
        {
            RefPtr<StateListDrawable> stateList = MakeRef<StateListDrawable>(MemoryAllocator());

            for (const StateImageEntry& entry : stateImages)
            {
                RefPtr<Drawable> drawable;
                if (slices.IsValid())
                    drawable = CreateNineSliceDrawable(entry.Name, slices, tint, expand);
                else
                    drawable = CreateImageDrawable(entry.Name, tint);

                if (drawable)
                    stateList->Set(entry.State, drawable);
            }

            return stateList;
        }

    private:
        image::ImageAtlasBuilder m_builder;
        bool m_built = false;
    };

    RTTI_DEFINE_OBJECT(ThemeAtlas, "rtti::ui")
}
