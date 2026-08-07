// Draconic::VG::SVG - :renderer partition.
//
// SVGRenderer: draws an SVGDocument to a VGContext (standalone, no UI framework).
// Scales the document to fit a bounds rect; an optional tint overrides all
// fill/stroke colors. Ported from Sedulous.VG.SVG/SVGRenderer.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg.svg:renderer;

import draconic.foundation;
import draconic.vg;
import draconic.fonts;
import :types;

using namespace draconic::foundation;

export namespace draconic::vg::svg
{
    /// Renders an SVGDocument to a VGContext.
    class SVGRenderer
    {
    public:
        /// Render scaled to fit `bounds`. `tint` (if set) overrides all colors.
        static void Render(draconic::vg::VGContext& vg, const SVGDocument& document,
                           Rectangle bounds, Optional<Color> tint = {})
        {
            if (document.elements.IsEmpty())
                return;

            const f32 scaleX = (document.width > 0.0f) ? bounds.width / document.width : 1.0f;
            const f32 scaleY = (document.height > 0.0f) ? bounds.height / document.height : 1.0f;

            vg.PushState();
            vg.Translate(bounds.x, bounds.y);
            vg.Scale(scaleX, scaleY);

            for (usize i = 0; i < document.elements.Size(); ++i)
                RenderElement(vg, document.elements[i], tint, &document);

            vg.PopState();
        }

        /// Render a single element and its children. `document` (optional) resolves
        /// fill="url(#id)" gradient references; without it those fall back to the
        /// element's fallback color.
        static void RenderElement(draconic::vg::VGContext& vg, const SVGElement& element,
                                  Optional<Color> tint = {},
                                  const SVGDocument* document = nullptr)
        {
            if (element.opacity <= 0.0f)
                return;

            vg.PushState();

            if (element.transform != Float4x4::Identity())
                vg.SetTransform(element.transform * vg.GetTransform());

            if (element.opacity < 1.0f)
                vg.PushOpacity(element.opacity);

            if (element.IsGroup())
            {
                for (usize i = 0; i < element.children.Size(); ++i)
                    RenderElement(vg, element.children[i], tint, document);
            }
            else if (element.type == SVGElementType::Text)
            {
                RenderText(vg, element, tint);
            }
            else if (element.path.HasValue())
            {
                // A tint (icon recolor) overrides everything, gradients included.
                const SVGGradient* gradient =
                    (!tint.HasValue() && document != nullptr &&
                     !element.fillGradientId.AsView().IsEmpty())
                        ? document->gradients.Find(element.fillGradientId)
                        : nullptr;
                if (gradient != nullptr && !gradient->stops.IsEmpty())
                    FillWithGradient(vg, element.path.Value(), *gradient);
                else if (element.fillColor.HasValue())
                    vg.FillPath(element.path.Value(),
                                tint.HasValue() ? tint.Value() : element.fillColor.Value());

                if (element.strokeColor.HasValue() && element.strokeWidth > 0.0f)
                    vg.StrokePath(element.path.Value(),
                                  tint.HasValue() ? tint.Value() : element.strokeColor.Value(),
                                  StrokeStyle(element.strokeWidth));
            }

            if (element.opacity < 1.0f)
                vg.PopOpacity();

            vg.PopState();
        }

    private:
        /// Build the VG fill for a gradient reference and fill the path with it.
        /// objectBoundingBox (the SVG default) maps the gradient's fractional geometry
        /// onto the path's bounds; userSpaceOnUse takes document coordinates directly
        /// (the VG transform already maps those to the screen).
        static void FillWithGradient(draconic::vg::VGContext& vg, const draconic::vg::Path& path,
                                     const SVGGradient& gradient)
        {
            const Rectangle b = path.GetBounds();
            if (gradient.radial)
            {
                draconic::vg::VGRadialGradientFill fill;
                if (gradient.userSpace)
                {
                    fill.center = Float2{gradient.cx, gradient.cy};
                    fill.radius = gradient.r;
                }
                else
                {
                    fill.center =
                        Float2{b.x + gradient.cx * b.width, b.y + gradient.cy * b.height};
                    // Spec: fractional radii scale by the bounds' normalized diagonal.
                    fill.radius = gradient.r *
                                  Sqrt((b.width * b.width + b.height * b.height) * 0.5f);
                }
                fill.spread = gradient.spread;
                fill.stops = gradient.stops;
                vg.FillPath(path, fill);
            }
            else
            {
                draconic::vg::VGLinearGradientFill fill;
                if (gradient.userSpace)
                {
                    fill.startPoint = Float2{gradient.x1, gradient.y1};
                    fill.endPoint = Float2{gradient.x2, gradient.y2};
                }
                else
                {
                    fill.startPoint =
                        Float2{b.x + gradient.x1 * b.width, b.y + gradient.y1 * b.height};
                    fill.endPoint =
                        Float2{b.x + gradient.x2 * b.width, b.y + gradient.y2 * b.height};
                }
                fill.spread = gradient.spread;
                fill.stops = gradient.stops;
                vg.FillPath(path, fill);
            }
        }

        static void RenderText(draconic::vg::VGContext& vg, const SVGElement& element,
                               Optional<Color> tint)
        {
            if (element.textContent.AsView().IsEmpty() || vg.FontService() == nullptr)
                return;

            // The current VG transform scales SVG units -> screen pixels. Glyphs are
            // rasterized at a fixed pixel size, so: compute the effective pixel font
            // size, convert position to screen space, and draw without the SVG scale.
            const Float4x4 transform = vg.GetTransform();
            const f32 scaleY =
                Sqrt(transform.m[1][0] * transform.m[1][0] + transform.m[1][1] * transform.m[1][1]);
            const f32 effectiveFontSize = element.fontSize * scaleY;

            draconic::fonts::CachedFont* font = vg.FontService()->GetFont(effectiveFontSize);
            if (font == nullptr)
                return;

            const Color color =
                tint.HasValue()
                    ? tint.Value()
                    : (element.fillColor.HasValue() ? element.fillColor.Value() : Color::Black);

            // SVG position -> screen pixels via the current transform.
            const f32 screenX = transform.m[0][0] * element.textX +
                                transform.m[1][0] * element.textY + transform.m[3][0];
            const f32 screenY = transform.m[0][1] * element.textX +
                                transform.m[1][1] * element.textY + transform.m[3][1];

            // text-anchor alignment in screen space.
            const f32 textW = font->font->MeasureString(element.textContent.AsView());
            f32 x = screenX;
            switch (element.textAnchor)
            {
            case SVGTextAnchor::Middle:
                x -= textW * 0.5f;
                break;
            case SVGTextAnchor::End:
                x -= textW;
                break;
            case SVGTextAnchor::Start:
                break;
            }

            // Draw with identity transform so glyph pixels aren't double-scaled.
            vg.PushState();
            vg.SetTransform(Float4x4::Identity());
            vg.DrawText(element.textContent.AsView(), font, Float2{x, screenY}, color);
            vg.PopState();
        }
    };
}
