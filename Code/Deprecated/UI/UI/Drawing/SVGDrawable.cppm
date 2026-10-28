// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :svg_drawable partition
//
// Renders SVG content via VGContext path operations (resolution-independent; ideal for icons).
// Thin wrapper around SVGRenderer. Ported from Sedulous.UI/src/Drawing/SVGDrawable.bf.
//
// Divergence (language): Our SVGLoader::Load returns Result<SVGDocument> (by value), so the
// document is held BY VALUE (m_document), not a heap pointer + ~delete. FromString returns a
// RefPtr<SVGDrawable> (empty on parse failure) instead of a raw pointer/null.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module foundation.ui:svg_drawable;

import foundation.core;
import foundation.image;   // Color, Rectangle, Float2, Optional, Result, RefPtr, String
import foundation.vg.svg; // SVGDocument, SVGLoader, SVGRenderer
import :drawable;
import :draw_context;

using namespace foundation::core;
namespace vg = foundation::vg;

export namespace foundation::ui
{
    class SVGDrawable : public Drawable
    {
        RTTI_OBJECT(SVGDrawable, Drawable)
    public:
        /// Optional tint; when set, overrides all stroke/fill colors in the SVG. Empty = original colors.
        Optional<Color> TintColor;

        explicit SVGDrawable(vg::svg::SVGDocument document) : m_document(Move(document)) {}

        /// Create from an SVG string. Returns an empty RefPtr on parse failure.
        [[nodiscard]] static RefPtr<SVGDrawable> FromString(IAllocator& allocator,
                                                    StringView svgContent)
        {
            Result<vg::svg::SVGDocument> result = vg::svg::SVGLoader::Load(svgContent);
            if (result.HasValue())
            {
                return MakeRef<SVGDrawable>(allocator, Move(result.Value()));
            }
            return {};
        }

        /// Create from an SVG string with a tint applied.
        [[nodiscard]] static RefPtr<SVGDrawable> FromString(IAllocator& allocator,
                                                    StringView svgContent, Color tint)
        {
            Result<vg::svg::SVGDocument> result = vg::svg::SVGLoader::Load(svgContent);
            if (result.HasValue())
            {
                RefPtr<SVGDrawable> d =
                    MakeRef<SVGDrawable>(allocator, Move(result.Value()));
                d->TintColor = tint;
                return d;
            }
            return {};
        }

        [[nodiscard]] Optional<Float2> IntrinsicSize() const override
        {
            if (m_document.width > 0.0f && m_document.height > 0.0f)
            {
                return Float2{m_document.width, m_document.height};
            }
            return {};
        }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            vg::svg::SVGRenderer::Render(ctx.VG(), m_document, bounds, TintColor);
        }

        /// The parsed document (the icon BAKER renders it into an atlas offline).
        [[nodiscard]] const vg::svg::SVGDocument& Document() const { return m_document; }

    private:
        vg::svg::SVGDocument m_document;
    };

    RTTI_DEFINE_OBJECT(SVGDrawable, "rtti::ui")

    /// SVGDrawable that PREFERS pre-baked bitmap variants: the Godot-verified crispness
    /// recipe (raster once at integer size with the AA baked into texels, draw as a
    /// pixel-snapped textured quad - every instance samples identical texels, no per-
    /// instance subpixel shimmer). Falls back to the live vector render until a baker
    /// supplies variants (headless/tests, or before the first bake), and again after
    /// ClearBakedVariants on a DPI change until the re-bake lands. Variants BORROW their
    /// atlas image - the baker owns it and must outlive the drawables' use.
    class BakedSVGDrawable : public SVGDrawable
    {
        RTTI_OBJECT(BakedSVGDrawable, SVGDrawable)
    public:
        struct BakedVariant
        {
            const foundation::image::ImageData* atlas = nullptr; // borrowed
            Rectangle srcRect;                                 // texel region in the atlas
            f32 sizePx = 0.0f;                                 // the square size it was baked at
        };

        explicit BakedSVGDrawable(vg::svg::SVGDocument document) : SVGDrawable(Move(document)) {}

        [[nodiscard]] static RefPtr<BakedSVGDrawable> FromString(IAllocator& allocator,
                                                    StringView svgContent)
        {
            Result<vg::svg::SVGDocument> result = vg::svg::SVGLoader::Load(svgContent);
            if (result.HasValue())
            {
                return MakeRef<BakedSVGDrawable>(allocator, Move(result.Value()));
            }
            return {};
        }

        void SetBakedVariants(Array<BakedVariant> variants) { m_variants = Move(variants); }
        void ClearBakedVariants() { m_variants.Clear(); }
        [[nodiscard]] bool HasBakedVariants() const { return !m_variants.IsEmpty(); }

        void Draw(UIDrawContext& ctx, const Rectangle& bounds) override
        {
            if (m_variants.IsEmpty())
            {
                SVGDrawable::Draw(ctx, bounds); // live vector fallback
                return;
            }
            // Nearest baked size to the DEVICE-pixel extent (bounds are logical; the VG
            // transform carries any DPI scale, and bake sizes are device pixels). Ties
            // prefer the LARGER bake (downscale softens, upscale blurs).
            const Float4x4& m = ctx.VG().GetTransform();
            const f32 sx = Length(Float2{m(0, 0), m(0, 1)});
            const f32 sy = Length(Float2{m(1, 0), m(1, 1)});
            const f32 scale = Max(0.0001f, Max(sx, sy));
            const f32 want = Max(bounds.width, bounds.height) * scale;
            const BakedVariant* best = &m_variants[0];
            for (const BakedVariant& v : m_variants)
            {
                const f32 dBest = Abs(best->sizePx - want);
                const f32 dThis = Abs(v.sizePx - want);
                if (dThis < dBest || (dThis == dBest && v.sizePx > best->sizePx))
                {
                    best = &v;
                }
            }
            ctx.VG().DrawImageSnapped(best->atlas, bounds, best->srcRect,
                                      TintColor ? *TintColor : Color::White);
        }

    private:
        Array<BakedVariant> m_variants;
    };

    RTTI_DEFINE_OBJECT(BakedSVGDrawable, "rtti::ui")
}
