// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Core - :content_fit partition
//
// ContentFit: how a content box (of `contentSize`) is placed inside an outer `region`
// through a FitMode, and the maps between region space and content space. Pure geometry:
// the outer rect is in region space (the caller's coordinate space), which is not
// necessarily a window. The renderer places the image with DstRect/SrcRect; input remaps
// points with ToContent/FromContent. Both share one computation, so they cannot drift.
//
// The content may be presented turned (`rotation`, clockwise in quarters): a landscape game on a
// portrait screen is shown turned. The fit is computed in the region turned back (a quarter turn
// swaps its sides), so a 1080 x 1920 region fits 1920 x 1080 content exactly; DstRect is the
// axis-aligned rectangle the turned content covers, and the point maps turn through it.

module;
#include "Core/Prelude.h"

export module foundation.core:content_fit;

import :base;
import :float2;
import :rectangle;

export namespace foundation::core
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

    // How the content is turned as it is presented, clockwise: a quarter turn shows its top on the
    // region's right, three quarters on its left.
    enum class ContentRotation
    {
        None,
        Quarter,
        Half,
        ThreeQuarter
    };

    // The shape a game is made for: Any takes the window's shape; Landscape or Portrait is turned to
    // fit a window of the other shape.
    enum class ScreenOrientation
    {
        Any,
        Landscape,
        Portrait
    };

    // The turn a game of `orientation` takes on a `width` x `height` window: none when it matches
    // (or takes any shape), else three quarters, the game's top on the window's left (a phone held
    // upright shows a landscape game as it shows on the phone turned counter-clockwise, Android's
    // primary landscape).
    [[nodiscard]] constexpr ContentRotation TurnFor(ScreenOrientation orientation, f32 width, f32 height) noexcept
    {
        const bool tall = height > width;
        const bool wide = width > height;
        if ((orientation == ScreenOrientation::Landscape && tall) ||
            (orientation == ScreenOrientation::Portrait && wide))
        {
            return ContentRotation::ThreeQuarter;
        }
        return ContentRotation::None;
    }

    struct ContentFit
    {
        Rectangle region = Rectangle{0, 0, 0, 0}; // outer rect, region space
        Float2 contentSize = Float2{0, 0};        // logical content resolution
        FitMode mode = FitMode::Stretch;
        ContentRotation rotation = ContentRotation::None;

        // Where the content is drawn within `region` (region-space). For Letterbox/IntegerScale
        // this is the centered, aspect-preserved sub-rect (the rest is bars); for Stretch/Crop it
        // is the whole region.
        [[nodiscard]] Rectangle DstRect() const noexcept
        {
            // The placement's rectangle in the turned-back frame, its corners turned onto the region.
            const Rectangle d = Compute().dst;
            const Float2 a = FromLocal(Float2{d.x, d.y});
            const Float2 b = FromLocal(Float2{d.x + d.width, d.y + d.height});
            const f32 x0 = a.x < b.x ? a.x : b.x;
            const f32 y0 = a.y < b.y ? a.y : b.y;
            const f32 x1 = a.x < b.x ? b.x : a.x;
            const f32 y1 = a.y < b.y ? b.y : a.y;
            return Rectangle{x0, y0, x1 - x0, y1 - y0};
        }

        // Which content texels are sampled ([0..contentSize]). For Crop this is the centered,
        // aspect-preserved slice; otherwise the whole content.
        [[nodiscard]] Rectangle SrcRect() const noexcept { return Compute().src; }

        // Region-space point -> content-space. Returns false when the point is outside the drawn
        // content (e.g. a letterbox bar): the "no hit" contract for input.
        [[nodiscard]] bool ToContent(Float2 pt, Float2& out) const noexcept
        {
            const Placement p = Compute();
            if (p.dst.width <= 0.0f || p.dst.height <= 0.0f)
            {
                return false;
            }
            const Float2 local = ToLocal(pt);
            if (!p.dst.Contains(local))
            {
                return false;
            }
            out = LocalToContent(p, local);
            return true;
        }

        // Region-space point -> content-space with no bound: over a letterbox bar it maps outside
        // the content rather than failing, for a pointer that keeps reporting wherever it is (a
        // window's whole input). A degenerate fit returns the point.
        [[nodiscard]] Float2 ToContentUnclamped(Float2 pt) const noexcept
        {
            const Placement p = Compute();
            if (p.dst.width <= 0.0f || p.dst.height <= 0.0f)
            {
                return pt;
            }
            return LocalToContent(p, ToLocal(pt));
        }

        // Content-space point -> region-space (inverse of ToContent). Used e.g. to place an IME
        // caret rect in window space for a text field inside a fitted surface.
        [[nodiscard]] Float2 FromContent(Float2 pt) const noexcept
        {
            const Placement p = Compute();
            const f32 rx = (p.src.width != 0.0f) ? (pt.x - p.src.x) / p.src.width : 0.0f;
            const f32 ry = (p.src.height != 0.0f) ? (pt.y - p.src.y) / p.src.height : 0.0f;
            return FromLocal(Float2{p.dst.x + rx * p.dst.width, p.dst.y + ry * p.dst.height});
        }

        // Region -> content scale factor (content units per region unit), per content axis (equal
        // for the aspect-preserving modes). For a relative movement (a mouse delta) use
        // ToContentDelta, which turns it as well.
        [[nodiscard]] Float2 Scale() const noexcept
        {
            const Placement p = Compute();
            return Float2{
                (p.dst.width != 0.0f) ? p.src.width / p.dst.width : 0.0f,
                (p.dst.height != 0.0f) ? p.src.height / p.dst.height : 0.0f,
            };
        }

        // A region-space movement (a mouse delta) in the content's units and frame: turned back with
        // the content, then scaled, so a look speed keeps its direction and is invariant to the
        // region's size.
        [[nodiscard]] Float2 ToContentDelta(Float2 delta) const noexcept
        {
            const Float2 turned = TurnBack(delta);
            const Float2 scale = Scale();
            return Float2{turned.x * scale.x, turned.y * scale.y};
        }

    private:
        // Whether the turn swaps the region's sides (a quarter either way).
        [[nodiscard]] bool SidesSwap() const noexcept
        {
            return rotation == ContentRotation::Quarter || rotation == ContentRotation::ThreeQuarter;
        }
        // The region turned back: the frame the content is fitted in, its origin at (0, 0).
        [[nodiscard]] Float2 LocalSize() const noexcept
        {
            return SidesSwap() ? Float2{region.height, region.width} : Float2{region.width, region.height};
        }
        // A direction in the region turned back into the local frame (no translation).
        [[nodiscard]] Float2 TurnBack(Float2 d) const noexcept
        {
            switch (rotation)
            {
            case ContentRotation::Quarter:
                return Float2{d.y, -d.x};
            case ContentRotation::Half:
                return Float2{-d.x, -d.y};
            case ContentRotation::ThreeQuarter:
                return Float2{-d.y, d.x};
            case ContentRotation::None:
            default:
                return d;
            }
        }
        // Region-space point -> the local frame (the region turned back).
        [[nodiscard]] Float2 ToLocal(Float2 pt) const noexcept
        {
            const f32 x = pt.x - region.x;
            const f32 y = pt.y - region.y;
            const Float2 size = LocalSize();
            switch (rotation)
            {
            case ContentRotation::Quarter:
                return Float2{y, size.y - x};
            case ContentRotation::Half:
                return Float2{size.x - x, size.y - y};
            case ContentRotation::ThreeQuarter:
                return Float2{size.x - y, x};
            case ContentRotation::None:
            default:
                return Float2{x, y};
            }
        }
        // The local frame -> region space (inverse of ToLocal).
        [[nodiscard]] Float2 FromLocal(Float2 pt) const noexcept
        {
            const Float2 size = LocalSize();
            switch (rotation)
            {
            case ContentRotation::Quarter:
                return Float2{region.x + size.y - pt.y, region.y + pt.x};
            case ContentRotation::Half:
                return Float2{region.x + size.x - pt.x, region.y + size.y - pt.y};
            case ContentRotation::ThreeQuarter:
                return Float2{region.x + pt.y, region.y + size.x - pt.x};
            case ContentRotation::None:
            default:
                return Float2{region.x + pt.x, region.y + pt.y};
            }
        }

        struct Placement
        {
            Rectangle dst;
            Rectangle src;
        };

        // A local-frame point -> content-space through a placement with a drawn area.
        [[nodiscard]] static Float2 LocalToContent(const Placement& p, Float2 local) noexcept
        {
            const f32 rx = (local.x - p.dst.x) / p.dst.width;
            const f32 ry = (local.y - p.dst.y) / p.dst.height;
            return Float2{p.src.x + rx * p.src.width, p.src.y + ry * p.src.height};
        }

        // The placement in the local frame (the region turned back, at the origin).
        [[nodiscard]] Placement Compute() const noexcept
        {
            const Float2 size = LocalSize();
            const Rectangle frame{0.0f, 0.0f, size.x, size.y};
            const f32 cw = contentSize.x, ch = contentSize.y;
            const Rectangle fullSrc{0.0f, 0.0f, cw, ch};
            if (cw <= 0.0f || ch <= 0.0f || frame.width <= 0.0f || frame.height <= 0.0f)
            {
                return Placement{frame, fullSrc};
            }
            const f32 sx = frame.width / cw;
            const f32 sy = frame.height / ch;

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
                const f32 dx = frame.x + (frame.width - dw) * 0.5f;
                const f32 dy = frame.y + (frame.height - dh) * 0.5f;
                return Placement{Rectangle{dx, dy, dw, dh}, fullSrc};
            }
            case FitMode::Crop:
            {
                const f32 s = (sx > sy) ? sx : sy;                        // fill, overflow cropped
                const f32 vw = frame.width / s, vh = frame.height / s;  // visible content size
                const f32 sxo = (cw - vw) * 0.5f, syo = (ch - vh) * 0.5f; // centered slice
                return Placement{frame, Rectangle{sxo, syo, vw, vh}};
            }
            case FitMode::Stretch:
            default:
                return Placement{frame, fullSrc};
            }
        }
    };
}
