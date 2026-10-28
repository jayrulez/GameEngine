// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :svg_drawable partition
//
// SvgDrawable: an SVG picture as image content (an Image widget's picture, a menu item's or a
// button's icon), at the size it lands in device pixels, crisp at any size. Its intrinsic size is
// the document's. A tint other than white recolours it whole, as VG's SVG renderer does (an icon
// taking its colour); white draws it as authored. With a picture baker it is baked once per size
// into the baker's atlas (white, when tinted: one bake for every tint) and drawn as one textured
// quad, pictures one after another one draw; as VG paths without one, until the bake lands, or
// when it is larger than an atlas keeps.
// It owns its document when made from SVG text (FromText: an icon set compiled into the binary,
// as the editor's are) and borrows the resource provider's for a source (PictureDrawable, which
// picks the kind by the source: an SVG by a .svg ending or a data:image/svg+xml URL, a raster
// otherwise).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.gui:svg_drawable;

import foundation.core;   // Color, Rectangle, Float2, Optional, RefPtr, Result
import foundation.vg.svg; // SVGDocument, SVGLoader, SVGRenderer
import :rect;
import :draw_context;
import :drawable;
import :image_drawable;
import :resource_provider;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    class SvgDrawable : public Drawable
    {
        RTTI_OBJECT(SvgDrawable, Drawable)
    public:
        /// Borrows a document that outlives it (the resource provider's).
        explicit SvgDrawable(const foundation::vg::svg::SVGDocument* document) noexcept : m_document(document) {}
        /// Owns a document (parsed from text).
        explicit SvgDrawable(foundation::vg::svg::SVGDocument document) noexcept
            : m_owned(core::Move(document)), m_document(&m_owned) {}
        SvgDrawable(const SvgDrawable&) = delete; // m_document may point at m_owned
        SvgDrawable& operator=(const SvgDrawable&) = delete;

        /// An SVG from its markup; null when it does not parse.
        [[nodiscard]] static RefPtr<SvgDrawable> FromText(IAllocator& allocator, core::StringView svg)
        {
            Result<foundation::vg::svg::SVGDocument> parsed = foundation::vg::svg::SVGLoader::Load(svg);
            if (!parsed.HasValue())
                return {};
            return core::MakeRef<SvgDrawable>(allocator, core::Move(parsed.Value()));
        }

        [[nodiscard]] const foundation::vg::svg::SVGDocument* Document() const noexcept { return m_document; }

        void Draw(DrawContext& ctx, const Rect& dest) override { DrawTinted(ctx, dest, Color::White); }

        void DrawTinted(DrawContext& ctx, const Rect& dest, Color tint) override
        {
            if (m_document == nullptr || dest.width <= 0.0f || dest.height <= 0.0f)
                return;
            const bool white = tint.r >= 1.0f && tint.g >= 1.0f && tint.b >= 1.0f && tint.a >= 1.0f;
            if (IPictureBaker* baker = ctx.PictureBaker())
            {
                const f32 scale = ctx.DeviceScale();
                const f32 width = Round(dest.width * scale), height = Round(dest.height * scale);
                if (width >= 1.0f && height >= 1.0f && width <= static_cast<f32>(IPictureBaker::kLargestAtlasPicture) &&
                    height <= static_cast<f32>(IPictureBaker::kLargestAtlasPicture))
                    if (const Optional<PictureRegion> region =
                            baker->AtlasRegion(*m_document, static_cast<u32>(width), static_cast<u32>(height), !white);
                        region.HasValue())
                    {
                        ctx.VG().DrawImagePremultiplied(region.Value().Page, dest.ToRectangle(), region.Value().Source,
                                                        white ? Color::White : tint);
                        return;
                    }
            }
            foundation::vg::svg::SVGRenderer::Render(ctx.VG(), *m_document, dest.ToRectangle(),
                                                     white ? Optional<Color>{} : Optional<Color>{tint});
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (m_document == nullptr || m_document->width <= 0.0f || m_document->height <= 0.0f)
                return {};
            return core::Float2{m_document->width, m_document->height};
        }

    private:
        foundation::vg::svg::SVGDocument m_owned;
        const foundation::vg::svg::SVGDocument* m_document = nullptr;
    };

    RTTI_DEFINE_OBJECT(SvgDrawable, "rtti::gui")

    /// Whether a source's last name has an ending (".png", ".svg"): a file's, not an id's.
    [[nodiscard]] inline bool HasFileEnding(core::StringView path) noexcept
    {
        for (usize i = path.Size(); i-- > 0;)
        {
            if (path[i] == u8'.')
                return true;
            if (path[i] == u8'/' || path[i] == u8'\\')
                return false;
        }
        return false;
    }

    /// A picture by its source, as a resource provider has it: an SvgDrawable for an SVG source
    /// (IsSvgSource: a .svg path or one written inline), an ImageDrawable otherwise, or for a source
    /// with no file ending (an id) whichever of the two the provider has; null when it has none.
    [[nodiscard]] inline RefPtr<Drawable> PictureDrawable(IResourceProvider& resources, core::StringView path, IAllocator& allocator)
    {
        if (IsSvgSource(path))
        {
            if (const foundation::vg::svg::SVGDocument* document = resources.Svg(path))
                return core::MakeRef<SvgDrawable>(allocator, document);
            return {};
        }
        if (const foundation::image::ImageData* image = resources.LoadImage(path))
            return core::MakeRef<ImageDrawable>(allocator, image);
        // A source with no file ending (a resource id, an engine's "{guid}") says nothing of its
        // kind: the provider knows, so a vector image named so is asked for as one.
        if (!HasFileEnding(path))
            if (const foundation::vg::svg::SVGDocument* document = resources.Svg(path))
                return core::MakeRef<SvgDrawable>(allocator, document);
        return {};
    }
}
