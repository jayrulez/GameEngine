// Draconic Foundation - :content_fit partition
//
// ContentFit: how a content box (of `contentSize`) is placed inside an outer `region`
// through a FitMode, and the maps between REGION-space and content-space. Pure geometry
// - the outer rect is region-space (the caller's coordinate space); it is NOT inherently
// a window. The renderer places the image with DstRect/SrcRect; input remaps points with
// ToContent/FromContent. One computation shared by both, so they can never drift.
// (See docs/design/viewport-input.md §4.3.)

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.foundation:content_fit;

import :base;
import :float2;
import :rectangle;

export namespace draconic::foundation
{
    // How content is scaled to fit its region:
    //  Stretch      - fill the region, ignore aspect (may distort).
    //  Letterbox    - preserve aspect, fit inside, bars on the short axis.
    //  Crop         - preserve aspect, fill the region, overflow cropped (source sliced).
    //  IntegerScale - like Letterbox but the scale is floored to a whole number (pixel-art).
    enum class FitMode
    {
        Stretch,
        Letterbox,
        Crop,
        IntegerScale
    };

    struct ContentFit
    {
        Rectangle region = Rectangle{0, 0, 0, 0}; // outer rect, REGION-space
        Float2 contentSize = Float2{0, 0};        // logical content resolution
        FitMode mode = FitMode::Stretch;

        // Where the content is drawn within `region` (region-space). For Letterbox/IntegerScale
        // this is the centered, aspect-preserved sub-rect (the rest is bars); for Stretch/Crop it
        // is the whole region.
        [[nodiscard]] Rectangle DstRect() const noexcept { return Compute().dst; }

        // Which content texels are sampled ([0..contentSize]). For Crop this is the centered,
        // aspect-preserved slice; otherwise the whole content.
        [[nodiscard]] Rectangle SrcRect() const noexcept { return Compute().src; }

        // Region-space point -> content-space. Returns false when the point is outside the drawn
        // content (e.g. a letterbox bar) - the "no hit" contract for input.
        [[nodiscard]] bool ToContent(Float2 pt, Float2& out) const noexcept
        {
            const Placement p = Compute();
            if (p.dst.width <= 0.0f || p.dst.height <= 0.0f)
            {
                return false;
            }
            if (!p.dst.Contains(pt))
            {
                return false;
            }
            const f32 rx = (pt.x - p.dst.x) / p.dst.width;
            const f32 ry = (pt.y - p.dst.y) / p.dst.height;
            out = Float2{p.src.x + rx * p.src.width, p.src.y + ry * p.src.height};
            return true;
        }

        // Content-space point -> region-space (inverse of ToContent). Used e.g. to place an IME
        // caret rect in window space for a text field inside a fitted surface.
        [[nodiscard]] Float2 FromContent(Float2 pt) const noexcept
        {
            const Placement p = Compute();
            const f32 rx = (p.src.width != 0.0f) ? (pt.x - p.src.x) / p.src.width : 0.0f;
            const f32 ry = (p.src.height != 0.0f) ? (pt.y - p.src.y) / p.src.height : 0.0f;
            return Float2{p.dst.x + rx * p.dst.width, p.dst.y + ry * p.dst.height};
        }

        // Region -> content scale factor (content units per region unit), for scaling relative
        // input (mouse delta) so sensitivity is invariant to region size. Per-axis (equal for the
        // aspect-preserving modes).
        [[nodiscard]] Float2 Scale() const noexcept
        {
            const Placement p = Compute();
            return Float2{
                (p.dst.width != 0.0f) ? p.src.width / p.dst.width : 0.0f,
                (p.dst.height != 0.0f) ? p.src.height / p.dst.height : 0.0f,
            };
        }

    private:
        struct Placement
        {
            Rectangle dst;
            Rectangle src;
        };

        [[nodiscard]] Placement Compute() const noexcept
        {
            const f32 cw = contentSize.x, ch = contentSize.y;
            const Rectangle fullSrc{0.0f, 0.0f, cw, ch};
            if (cw <= 0.0f || ch <= 0.0f || region.width <= 0.0f || region.height <= 0.0f)
            {
                return Placement{region, fullSrc};
            }
            const f32 sx = region.width / cw;
            const f32 sy = region.height / ch;

            switch (mode)
            {
            case FitMode::Letterbox:
            case FitMode::IntegerScale:
            {
                f32 s = (sx < sy) ? sx : sy; // fit inside
                if (mode == FitMode::IntegerScale)
                {
                    s = static_cast<f32>(static_cast<i32>(s)); // floor (s > 0)
                    if (s < 1.0f)
                    {
                        s = 1.0f;
                    }
                }
                const f32 dw = cw * s, dh = ch * s;
                const f32 dx = region.x + (region.width - dw) * 0.5f;
                const f32 dy = region.y + (region.height - dh) * 0.5f;
                return Placement{Rectangle{dx, dy, dw, dh}, fullSrc};
            }
            case FitMode::Crop:
            {
                const f32 s = (sx > sy) ? sx : sy;                        // fill, overflow cropped
                const f32 vw = region.width / s, vh = region.height / s;  // visible content size
                const f32 sxo = (cw - vw) * 0.5f, syo = (ch - vh) * 0.5f; // centered slice
                return Placement{region, Rectangle{sxo, syo, vw, vh}};
            }
            case FitMode::Stretch:
            default:
                return Placement{region, fullSrc};
            }
        }
    };
}
