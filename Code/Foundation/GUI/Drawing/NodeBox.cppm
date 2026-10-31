// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :node_box partition
//
// NodeBox: a node's box as values resolved to pixels. It holds the background colour and layers
// (gradients, images), a border width and colour per side, corner radii and box shadows, plus the
// effects drawn with them: a blur of the node with its subtree, a blur of what lies behind the box,
// and how the node blends into what is under it. A sheet's box properties write it, and native code
// may set it directly. The node draws it through VG (analytic boxes, box shadows, gradient fills)
// without allocating per draw or per restyle, and transitions interpolate its fields.

module;
#include "Core/Prelude.h"

export module foundation.gui:node_box;

import foundation.core;
import foundation.image;
import foundation.vg;
import foundation.vg.svg; // SVGDocument, SVGRenderer (an SVG layer)
import :rect;
import :draw_context;
import :picture_baker; // baking an SVG skin

using namespace foundation::core;
namespace core = foundation::core;
namespace vg = foundation::vg;
namespace image = foundation::image;

export namespace foundation::gui
{
    enum class BoxGradientKind : u8
    {
        Linear,
        Radial,
        Conic,
    };

    /// A gradient layer, its stops settled to positions in [0, 1].
    struct BoxGradient
    {
        BoxGradientKind Kind = BoxGradientKind::Linear;
        f32 Angle = 3.14159265358979f; ///< Linear: CSS's angle (0 up, clockwise). Conic: the start angle.
        bool Repeating = false;
        Array<vg::GradientStop> Stops;
    };

    /// A layer's size or position along one axis: pixels plus a percentage of a base known when
    /// drawing, or auto.
    struct LayerLength
    {
        f32 Px = 0.0f;
        f32 Percent = 0.0f;
        bool Auto = false;
        [[nodiscard]] f32 Resolve(f32 base) const noexcept { return Px + Percent * base * 0.01f; }
    };
    /// How an image layer is sized in its box (CSS's background-size). Fill, which fills the box, is
    /// used when none is given.
    enum class LayerSize : u8
    {
        Fill,
        Auto,    ///< the image's own size
        Cover,   ///< the smallest size that covers the box, keeping the aspect ratio
        Contain, ///< the largest size that fits in the box, keeping the aspect ratio
        Lengths, ///< Width and Height; one that is auto follows from the other by the aspect ratio
    };

    /// One background layer over the background colour: a gradient or an image.
    struct BoxLayer
    {
        bool IsImage = false;
        BoxGradient Gradient;
        const image::ImageData* Image = nullptr; ///< Not owned (the resource provider's).
        /// An SVG picture instead of Image: drawn as paths at the size it lands, crisp at any
        /// scale. Not owned (the resource provider's).
        const foundation::vg::svg::SVGDocument* Svg = nullptr;
        /// The region of Image to draw (a sprite of an atlas, `url(atlas.png#xywh=x,y,w,h)`); empty
        /// draws the whole image.
        Rectangle Source{};
        LayerSize Size = LayerSize::Fill;
        LayerLength Width{0.0f, 0.0f, true};
        LayerLength Height{0.0f, 0.0f, true};
        /// background-position: from the box's left and top, a percentage of the room left over
        /// (the box minus the image: 50% centres it, 100% puts it at the far edge).
        LayerLength X{};
        LayerLength Y{};
    };

    /// How a border side is drawn (CSS's border-style; the styles the GUI does not draw are solid).
    enum class BorderLine : u8
    {
        Solid,
        Dashed, ///< dashes three times the border width, with gaps twice the width
        Dotted, ///< round dots as wide as the border, spaced as far apart
    };

    /// How a border image fills an edge (CSS's border-image-repeat).
    enum class BorderImageRepeat : u8
    {
        Stretch, ///< one piece stretched along the edge
        Repeat,  ///< tiles from the middle outward, cut at the ends
        Round,   ///< whole tiles, stretched so a whole number of them fits
    };
    /// A nine-slice skin over the box (CSS's border-image). Slice cuts the image into corners, edges
    /// and a middle, which are drawn into Width from the box's edges in place of the border; the
    /// middle is drawn too when Fill is set. The image is a raster or an SVG; an SVG's pieces are
    /// drawn as paths at the size they land.
    struct BorderImage
    {
        const image::ImageData* Image = nullptr; ///< Not owned (the resource provider's).
        const foundation::vg::svg::SVGDocument* Svg = nullptr; ///< instead of Image; not owned (the resource provider's)
        Rectangle Source{};                      ///< the region of Image to use (a sprite); empty uses all of it
        f32 Slice[4]{};                          ///< top, right, bottom, left, in the image's pixels (an SVG's units)
        f32 Width[4]{-1.0f, -1.0f, -1.0f, -1.0f}; ///< top, right, bottom, left, in pixels; below 0 uses the border's width
        bool Fill = false;
        BorderImageRepeat RepeatX = BorderImageRepeat::Stretch; ///< for the top, bottom and middle, horizontally
        BorderImageRepeat RepeatY = BorderImageRepeat::Stretch; ///< for the left, right and middle, vertically

        [[nodiscard]] bool HasPicture() const noexcept { return Image != nullptr || Svg != nullptr; }
    };

    /// Returns where an image of `iw` by `ih` pixels lands for `layer` in `box`: sized by the
    /// layer's background-size and placed by its background-position. It may reach past the box,
    /// where drawing cuts it off. Returns an empty rect for an image or a box of no size.
    [[nodiscard]] inline Rectangle PlaceImageLayer(const BoxLayer& layer, const Rectangle& box, f32 iw, f32 ih) noexcept
    {
        if (iw <= 0.0f || ih <= 0.0f || box.width <= 0.0f || box.height <= 0.0f)
            return Rectangle{};
        f32 w = box.width, h = box.height;
        switch (layer.Size)
        {
        case LayerSize::Fill: return box;
        case LayerSize::Auto:
            w = iw;
            h = ih;
            break;
        case LayerSize::Cover:
        case LayerSize::Contain:
        {
            const f32 sx = box.width / iw, sy = box.height / ih;
            const f32 scale = layer.Size == LayerSize::Cover ? Max(sx, sy) : Min(sx, sy);
            w = iw * scale;
            h = ih * scale;
            break;
        }
        case LayerSize::Lengths:
            w = layer.Width.Auto ? -1.0f : layer.Width.Resolve(box.width);
            h = layer.Height.Auto ? -1.0f : layer.Height.Resolve(box.height);
            if (w < 0.0f && h < 0.0f)
            {
                w = iw;
                h = ih;
            }
            else if (w < 0.0f)
                w = h * iw / ih;
            else if (h < 0.0f)
                h = w * ih / iw;
            break;
        }
        if (w <= 0.0f || h <= 0.0f)
            return Rectangle{};
        return Rectangle{box.x + layer.X.Resolve(box.width - w), box.y + layer.Y.Resolve(box.height - h), w, h};
    }

    /// One box shadow, in pixels.
    struct BoxShadow
    {
        core::Float2 Offset{};
        f32 Blur = 0.0f;
        f32 Spread = 0.0f;
        Color ShadowColor{0.0f, 0.0f, 0.0f, 0.5f};
        bool Inset = false;

        [[nodiscard]] bool operator==(const BoxShadow&) const noexcept = default;
    };

    struct NodeBox
    {
        Color BackgroundColor{0.0f, 0.0f, 0.0f, 0.0f};
        Array<BoxLayer> Layers; ///< First on top, as CSS lists them.
        BorderImage Skin;       ///< border-image: drawn in place of the border when it has an image
        BorderLine BorderLines[4]{}; ///< top, right, bottom, left (dashed and dotted)
        vg::BorderWidths BorderWidths{};
        vg::BorderColors BorderColors{};
        vg::CornerRadii Radii{};
        Array<BoxShadow> Shadows;
        f32 FilterBlur = 0.0f;   ///< filter: blur(): the node and its subtree blur as one (a sigma, logical units)
        f32 BackdropBlur = 0.0f; ///< backdrop-filter: blur(): blurs what this canvas drew behind the box
        vg::VGBlendMode BlendMode = vg::VGBlendMode::Normal; ///< mix-blend-mode: how the node composites
        /// text-shadow, text-outline, text-glow: drawn under the text of this node and its children,
        /// inherited through the VG state as CSS's text-shadow is. Each effect the node sets
        /// (TextEffectsSet's bits, kTextShadow and the rest) replaces its parent's.
        vg::TextEffects TextEffects{};
        u8 TextEffectsSet = 0;
        static constexpr u8 kTextShadow = 1, kTextOutline = 2, kTextGlow = 4;

        /// Returns the text effects this node draws under: `inherited`, with the effects this node
        /// sets replaced.
        [[nodiscard]] vg::TextEffects MergeTextEffects(vg::TextEffects inherited) const noexcept
        {
            if ((TextEffectsSet & kTextShadow) != 0)
            {
                inherited.shadowOffset = TextEffects.shadowOffset;
                inherited.shadowBlur = TextEffects.shadowBlur;
                inherited.shadowColor = TextEffects.shadowColor;
            }
            if ((TextEffectsSet & kTextOutline) != 0)
            {
                inherited.outlineWidth = TextEffects.outlineWidth;
                inherited.outlineColor = TextEffects.outlineColor;
            }
            if ((TextEffectsSet & kTextGlow) != 0)
            {
                inherited.glowRadius = TextEffects.glowRadius;
                inherited.glowColor = TextEffects.glowColor;
            }
            return inherited;
        }

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return BackgroundColor.a <= 0.0f && Layers.IsEmpty() && BorderWidths.IsZero() && Shadows.IsEmpty() &&
                   BackdropBlur <= 0.0f && !Skin.HasPicture();
        }
        /// Whether the node must draw through a layer of its own: a filter or a blend applies to
        /// the node and its subtree as one picture.
        [[nodiscard]] bool NeedsLayer() const noexcept
        {
            return FilterBlur > 0.0f || BlendMode != vg::VGBlendMode::Normal;
        }

        /// Draws what goes under the content: outer shadows, the backdrop blur, the background
        /// colour, the layers (the last first), inset shadows, then the border.
        void DrawUnder(DrawContext& ctx, const Rect& box, bool withBackdrop = true) const
        {
            if (IsEmpty() || box.width <= 0.0f || box.height <= 0.0f)
                return;
            vg::VGContext& canvas = ctx.VG();
            const Rectangle rect{box.x, box.y, box.width, box.height};
            for (const BoxShadow& shadow : Shadows)
            {
                if (shadow.Inset)
                    continue;
                // A sharp ring round the box (a focus ring: spread, no blur, no offset) covers only
                // the band outside the box, as CSS clips an outer shadow to the outside. So a box with
                // no background (a field inside a frame) shows what is behind it, not the ring's
                // colour. The ring is stroked along the band's middle, so nothing is painted inside.
                if (shadow.Blur <= 0.0f && shadow.Offset.x == 0.0f && shadow.Offset.y == 0.0f && shadow.Spread > 0.0f)
                {
                    const f32 half = shadow.Spread * 0.5f;
                    const Rectangle middle{rect.x - half, rect.y - half, rect.width + shadow.Spread, rect.height + shadow.Spread};
                    vg::PathBuilder band;
                    vg::ShapeBuilder::BuildRoundedRect(middle, Grown(Radii, half), band);
                    canvas.StrokePath(band.ToPath(), shadow.ShadowColor,
                                      vg::StrokeStyle(shadow.Spread, vg::VGLineCap::Butt, vg::VGLineJoin::Round));
                    continue;
                }
                const Rectangle grown{rect.x + shadow.Offset.x - shadow.Spread, rect.y + shadow.Offset.y - shadow.Spread,
                                      rect.width + 2.0f * shadow.Spread, rect.height + 2.0f * shadow.Spread};
                canvas.FillBoxShadow(grown, Grown(Radii, shadow.Spread), shadow.Blur, shadow.ShadowColor, false);
            }
            // The backdrop blurs inside the box's shape, under its (translucent) background.
            if (withBackdrop)
                DrawBackdrop(ctx, box);
            // A background directly under a solid border is drawn as one box with the border. Drawn
            // separately, both would be smoothed along the same outer curve, and the background
            // would show outside the border at a rounded corner. One box takes its fill and border
            // from one edge.
            const bool bordered = !Skin.HasPicture() && !BorderWidths.IsZero() && !BorderColors.IsTransparent();
            const bool solid = BorderLines[0] == BorderLine::Solid && BorderLines[1] == BorderLine::Solid &&
                               BorderLines[2] == BorderLine::Solid && BorderLines[3] == BorderLine::Solid;
            bool insetShadows = false;
            for (const BoxShadow& shadow : Shadows)
                insetShadows = insetShadows || shadow.Inset;
            if (bordered && solid && BackgroundColor.a > 0.0f && Layers.IsEmpty() && !insetShadows)
            {
                canvas.DrawBox(rect, Radii, BackgroundColor, BorderWidths, BorderColors);
                return;
            }
            if (BackgroundColor.a > 0.0f)
                canvas.DrawBox(rect, Radii, BackgroundColor);
            for (usize i = Layers.Size(); i-- > 0;)
                DrawLayer(canvas, Layers[i], rect);
            for (const BoxShadow& shadow : Shadows)
            {
                if (!shadow.Inset)
                    continue;
                const Rectangle shrunk{rect.x + shadow.Offset.x + shadow.Spread, rect.y + shadow.Offset.y + shadow.Spread,
                                       rect.width - 2.0f * shadow.Spread, rect.height - 2.0f * shadow.Spread};
                canvas.FillBoxShadow(shrunk, Grown(Radii, -shadow.Spread), shadow.Blur, shadow.ShadowColor, true);
            }
            if (Skin.HasPicture())
                DrawSkin(ctx, rect); // a border image stands in for the border (CSS)
            else if (bordered)
            {
                if (solid)
                    canvas.DrawBox(rect, Radii, Color{0.0f, 0.0f, 0.0f, 0.0f}, BorderWidths, BorderColors);
                else
                    DrawLinedBorder(canvas, rect);
            }
        }

        /// Draws only the backdrop blur. A node drawing through a layer draws this first, behind the
        /// layer.
        void DrawBackdrop(DrawContext& ctx, const Rect& box) const
        {
            if (BackdropBlur > 0.0f && box.width > 0.0f && box.height > 0.0f)
                ctx.VG().DrawBackdropBlur(Rectangle{box.x, box.y, box.width, box.height}, Radii, BackdropBlur * ctx.DeviceScale());
        }

    private:
        [[nodiscard]] static vg::CornerRadii Grown(const vg::CornerRadii& r, f32 by) noexcept
        {
            const auto grow = [by](f32 radius) { return radius > 0.0f ? Max(0.0f, radius + by) : 0.0f; };
            return vg::CornerRadii(grow(r.topLeft), grow(r.topRight), grow(r.bottomRight), grow(r.bottomLeft));
        }

        // Draws an image layer sized and placed in the box, cutting off the part outside it. The
        // source is cropped to match, so nothing spills past the box or its corners.
        void DrawImageLayer(vg::VGContext& canvas, const BoxLayer& layer, const Rectangle& rect) const
        {
            const bool region = layer.Source.width > 0.0f && layer.Source.height > 0.0f; // a sprite
            const f32 iw = region ? layer.Source.width : static_cast<f32>(layer.Image->Width());
            const f32 ih = region ? layer.Source.height : static_cast<f32>(layer.Image->Height());
            const f32 ix = region ? layer.Source.x : 0.0f;
            const f32 iy = region ? layer.Source.y : 0.0f;
            const Rectangle placed = PlaceImageLayer(layer, rect, iw, ih);
            if (placed.width <= 0.0f || placed.height <= 0.0f)
                return;
            const Rectangle shown = Rectangle::Intersect(placed, rect);
            if (shown.width <= 0.0f || shown.height <= 0.0f)
                return;
            const Rectangle source{ix + (shown.x - placed.x) / placed.width * iw, iy + (shown.y - placed.y) / placed.height * ih,
                                   shown.width / placed.width * iw, shown.height / placed.height * ih};
            if (Radii.topLeft > 0.0f || Radii.topRight > 0.0f || Radii.bottomRight > 0.0f || Radii.bottomLeft > 0.0f)
                canvas.DrawImageRounded(layer.Image, shown, source, Radii);
            else
                canvas.DrawImage(layer.Image, shown, source, Color::White);
        }

        // Draws a border with dashed or dotted sides, as browsers draw one. Each side is a path down
        // the middle of its band: from the half-way point of its first corner's arc, along its
        // edge, to the half-way point of the next corner's arc (at a square corner, the corner
        // itself). The dashes or dots are fitted to that length, so the side starts and ends on
        // one. Dots are as long as the border is wide, with gaps as long, and are round once the
        // border is 3 wide. Each side is clipped to its trapezoid, so sides of different styles or
        // colours meet on the corner's diagonal.
        void DrawLinedBorder(vg::VGContext& canvas, const Rectangle& rect) const
        {
            const f32 widths[4] = {BorderWidths.top, BorderWidths.right, BorderWidths.bottom, BorderWidths.left};
            const Color colors[4] = {BorderColors.top, BorderColors.right, BorderColors.bottom, BorderColors.left};
            const f32 l = rect.x, t = rect.y, r = rect.x + rect.width, b = rect.y + rect.height;
            // The corners clockwise from the top left, and each side between corners i and i + 1.
            const f32 radii[4] = {Radii.topLeft, Radii.topRight, Radii.bottomRight, Radii.bottomLeft};
            // Each side's own region: its outer edge, and the corners' diagonals (from the outer
            // corner through the inner corner), carried in far enough to hold half of each corner
            // arc.
            f32 deepest = 0.0f, thinnest = 1e9f;
            for (usize k = 0; k < 4; ++k)
            {
                deepest = Max(deepest, radii[k] + widths[k]);
                if (widths[k] > 0.0f)
                    thinnest = Min(thinnest, widths[k]);
            }
            const f32 reach = Min(Max(1.0f, deepest / Max(thinnest, 0.5f)), 0.5f * Min(rect.width, rect.height) / Max(thinnest, 0.5f));
            const f32 il = l + widths[3] * reach, it = t + widths[0] * reach, ir = r - widths[1] * reach, ib = b - widths[2] * reach;
            const core::Float2 shapes[4][4] = {
                {{l, t}, {r, t}, {ir, it}, {il, it}},
                {{r, t}, {r, b}, {ir, ib}, {ir, it}},
                {{r, b}, {l, b}, {il, ib}, {ir, ib}},
                {{l, b}, {l, t}, {il, it}, {il, ib}},
            };
            const core::Float2 corners[4] = {{l, t}, {r, t}, {r, b}, {l, b}};
            const core::Float2 inward[4] = {{1.0f, 1.0f}, {-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}}; // the inward direction at each corner, where its arc's centre lies
            for (usize i = 0; i < 4; ++i)
            {
                if (widths[i] <= 0.0f || colors[i].a <= 0.0f)
                    continue;
                const f32 w = widths[i];
                const usize c0 = i, c1 = (i + 1) % 4;
                // Build the side's path and measure its length.
                vg::PathBuilder path;
                f32 length = 0.0f;
                const auto along = [&](usize corner, bool start) -> core::Float2
                {
                    // Where the side's middle line meets the straight run at `corner` (at a square
                    // corner, the corner itself).
                    const f32 R = radii[corner];
                    const core::Float2 c = corners[corner];
                    const core::Float2 in = inward[corner];
                    const bool horizontal = i % 2 == 0;
                    const f32 off = w * 0.5f;
                    const f32 run = R > off ? R : 0.0f;
                    (void)start;
                    return horizontal ? core::Float2{c.x + in.x * run, c.y + in.y * off} : core::Float2{c.x + in.x * off, c.y + in.y * run};
                };
                const auto halfway = [&](usize corner) -> core::Float2
                {
                    const f32 R = radii[corner];
                    const core::Float2 c = corners[corner];
                    const core::Float2 in = inward[corner];
                    const f32 rc = R - w * 0.5f;
                    const core::Float2 centre{c.x + in.x * R, c.y + in.y * R};
                    return core::Float2{centre.x - in.x * rc * 0.70710678f, centre.y - in.y * rc * 0.70710678f};
                };
                constexpr f32 kEighth = 0.78539816f; // half a quarter circle, in radians
                const f32 rc0 = radii[c0] - w * 0.5f, rc1 = radii[c1] - w * 0.5f;
                const core::Float2 runStart = along(c0, true), runEnd = along(c1, false);
                if (rc0 > 0.0f)
                {
                    path.MoveTo(halfway(c0));
                    path.ArcTo(rc0, rc0, 0.0f, false, true, runStart);
                    length += rc0 * kEighth;
                }
                else
                    path.MoveTo(runStart);
                path.LineTo(runEnd);
                length += Max(Abs(runEnd.x - runStart.x), Abs(runEnd.y - runStart.y));
                if (rc1 > 0.0f)
                {
                    path.ArcTo(rc1, rc1, 0.0f, false, true, halfway(c1));
                    length += rc1 * kEighth;
                }
                // Fit the dash pattern to the side's length.
                vg::StrokeStyle style(w);
                f32 dash[2] = {0.0f, 0.0f};
                usize dashes = 0;
                if (BorderLines[i] != BorderLine::Solid && length > 0.0f)
                {
                    const bool round = BorderLines[i] == BorderLine::Dotted && w >= 3.0f;
                    const f32 piece = BorderLines[i] == BorderLine::Dashed ? 3.0f * w : round ? 0.0f : w;
                    const f32 gap = BorderLines[i] == BorderLine::Dashed ? 2.0f * w : round ? 2.0f * w : w;
                    const f32 n = Max(1.0f, static_cast<f32>(static_cast<i32>((length + gap) / (piece + gap) + 0.5f)));
                    if (n > 1.0f && length > piece)
                    {
                        dash[0] = Max(piece, 0.001f);
                        dash[1] = Max((length - piece * n) / (n - 1.0f), 0.001f);
                        dashes = 2;
                    }
                    if (round)
                        style.cap = vg::VGLineCap::Round;
                }
                vg::PathBuilder trapezoid;
                trapezoid.MoveTo(shapes[i][0]);
                for (usize k = 1; k < 4; ++k)
                    trapezoid.LineTo(shapes[i][k]);
                trapezoid.Close();
                canvas.PushClipPath(trapezoid.ToPath());
                canvas.StrokePath(path.ToPath(), colors[i], style, Span<const f32>(dash, dashes));
                canvas.PopClipPath();
            }
        }

        // Draws the border image: nine pieces of the image (or of its region) into the box. Corners
        // are drawn as they are cut, and the edges and the middle are stretched, repeated or rounded
        // along. When the host bakes, an SVG's pieces come from the picture baked at the size its
        // corners land, each drawn as a textured quad. Until then, and without a baker, each piece
        // is the document drawn as paths.
        void DrawSkin(DrawContext& ctx, const Rectangle& rect) const
        {
            vg::VGContext& canvas = ctx.VG();
            Rectangle src = Skin.Source.width > 0.0f && Skin.Source.height > 0.0f ? Skin.Source
                            : Skin.Image != nullptr
                                ? Rectangle{0.0f, 0.0f, static_cast<f32>(Skin.Image->Width()), static_cast<f32>(Skin.Image->Height())}
                                : Rectangle{0.0f, 0.0f, Skin.Svg->width, Skin.Svg->height};
            const f32 borders[4] = {BorderWidths.top, BorderWidths.right, BorderWidths.bottom, BorderWidths.left};
            f32 w[4];
            for (usize i = 0; i < 4; ++i)
                w[i] = Skin.Width[i] >= 0.0f ? Skin.Width[i] : borders[i];
            // Widths too big for the box shrink together (CSS's rule).
            const f32 fitX = w[1] + w[3] > rect.width && w[1] + w[3] > 0.0f ? rect.width / (w[1] + w[3]) : 1.0f;
            const f32 fitY = w[0] + w[2] > rect.height && w[0] + w[2] > 0.0f ? rect.height / (w[0] + w[2]) : 1.0f;
            const f32 fit = Min(fitX, fitY);
            for (f32& v : w)
                v *= fit;
            f32 s[4] = {Min(Skin.Slice[0], src.height), Min(Skin.Slice[1], src.width), Min(Skin.Slice[2], src.height),
                        Min(Skin.Slice[3], src.width)};
            const image::ImageData* baked = Skin.Svg != nullptr ? BakedSkin(ctx, w, s) : nullptr;
            if (baked != nullptr)
            {
                // Measure the slices in the baked picture's pixels instead of the document's units.
                const f32 px = static_cast<f32>(baked->Width()) / Skin.Svg->width;
                const f32 py = static_cast<f32>(baked->Height()) / Skin.Svg->height;
                src = Rectangle{0.0f, 0.0f, static_cast<f32>(baked->Width()), static_cast<f32>(baked->Height())};
                s[0] *= py;
                s[2] *= py;
                s[1] *= px;
                s[3] *= px;
            }
            // The column and row edges, in the box and in the image.
            const f32 dx[4] = {rect.x, rect.x + w[3], rect.x + rect.width - w[1], rect.x + rect.width};
            const f32 dy[4] = {rect.y, rect.y + w[0], rect.y + rect.height - w[2], rect.y + rect.height};
            const f32 sx[4] = {src.x, src.x + s[3], src.x + src.width - s[1], src.x + src.width};
            const f32 sy[4] = {src.y, src.y + s[0], src.y + src.height - s[2], src.y + src.height};
            for (usize row = 0; row < 3; ++row)
                for (usize col = 0; col < 3; ++col)
                {
                    if (row == 1 && col == 1 && !Skin.Fill)
                        continue;
                    const Rectangle d{dx[col], dy[row], dx[col + 1] - dx[col], dy[row + 1] - dy[row]};
                    const Rectangle piece{sx[col], sy[row], sx[col + 1] - sx[col], sy[row + 1] - sy[row]};
                    if (d.width <= 0.0f || d.height <= 0.0f || piece.width <= 0.0f || piece.height <= 0.0f)
                        continue;
                    // An edge's tile is as tall (or wide) as the edge, with its length scaled by the
                    // same factor.
                    const BorderImageRepeat along = col == 1 ? Skin.RepeatX : BorderImageRepeat::Stretch;
                    const BorderImageRepeat down = row == 1 ? Skin.RepeatY : BorderImageRepeat::Stretch;
                    const f32 tileW = col == 1 ? piece.width * (row == 1 ? (w[0] > 0.0f ? w[0] / Max(s[0], 1.0f) : 1.0f) : d.height / piece.height) : d.width;
                    const f32 tileH = row == 1 ? piece.height * (col == 1 ? (w[3] > 0.0f ? w[3] / Max(s[3], 1.0f) : 1.0f) : d.width / piece.width) : d.height;
                    DrawTiled(canvas, d, piece, along, down, tileW, tileH, baked);
                }
        }
        // Returns the SVG skin baked at its corners' scale, in device pixels. The scale is the
        // largest ratio of border width to slice, so no corner is magnified. Returns null when there
        // is no baker, when the skin is not baked yet (asking queues the bake), or when it would be
        // larger than a bake is allowed.
        const image::ImageData* BakedSkin(DrawContext& ctx, const f32 (&w)[4], const f32 (&s)[4]) const
        {
            static constexpr f32 kLargestBake = 2048.0f;
            IPictureBaker* baker = ctx.PictureBaker();
            if (baker == nullptr || Skin.Svg->width <= 0.0f || Skin.Svg->height <= 0.0f)
                return nullptr;
            f32 scale = 0.0f;
            for (usize i = 0; i < 4; ++i)
                if (s[i] > 0.0f)
                    scale = Max(scale, w[i] / s[i]);
            scale = (scale > 0.0f ? scale : 1.0f) * ctx.DeviceScale();
            const f32 width = Ceil(Skin.Svg->width * scale), height = Ceil(Skin.Svg->height * scale);
            if (width < 1.0f || height < 1.0f || width > kLargestBake || height > kLargestBake)
                return nullptr;
            return baker->Baked(*Skin.Svg, static_cast<u32>(width), static_cast<u32>(height));
        }
        // Draws `piece` of the skin's image over `d`, stretched or tiled at `tileW` by `tileH`.
        // Repeat centres the tiles and cuts them at the ends; round fits a whole number of tiles,
        // stretched.
        void DrawTiled(vg::VGContext& canvas, const Rectangle& d, const Rectangle& piece, BorderImageRepeat along,
                       BorderImageRepeat down, f32 tileW, f32 tileH, const image::ImageData* baked) const
        {
            const auto layout = [](BorderImageRepeat mode, f32 length, f32 tile, f32& size, f32& start, usize& count)
            {
                if (mode == BorderImageRepeat::Stretch || tile <= 0.5f)
                {
                    size = length;
                    start = 0.0f;
                    count = 1;
                    return;
                }
                if (mode == BorderImageRepeat::Round)
                {
                    count = static_cast<usize>(Max(1.0f, static_cast<f32>(static_cast<i32>(length / tile + 0.5f))));
                    size = length / static_cast<f32>(count);
                    start = 0.0f;
                    return;
                }
                size = tile;
                count = static_cast<usize>(length / tile) + 2; // centred, with a partial tile at each end
                start = (length - tile * static_cast<f32>(count)) * 0.5f;
            };
            f32 tw, th, x0, y0;
            usize nx, ny;
            layout(along, d.width, tileW, tw, x0, nx);
            layout(down, d.height, tileH, th, y0, ny);
            for (usize j = 0; j < ny; ++j)
                for (usize i = 0; i < nx; ++i)
                {
                    const Rectangle tile{d.x + x0 + tw * static_cast<f32>(i), d.y + y0 + th * static_cast<f32>(j), tw, th};
                    const Rectangle shown = Rectangle::Intersect(tile, d);
                    if (shown.width <= 0.0f || shown.height <= 0.0f)
                        continue;
                    const Rectangle part{piece.x + (shown.x - tile.x) / tw * piece.width, piece.y + (shown.y - tile.y) / th * piece.height,
                                         shown.width / tw * piece.width, shown.height / th * piece.height};
                    if (Skin.Image != nullptr)
                        canvas.DrawImage(Skin.Image, shown, part, Color::White);
                    else if (baked != nullptr)
                        canvas.DrawImagePremultiplied(baked, shown, part);
                    else
                        DrawSvgPart(canvas, shown, part);
                }
        }
        // Draws `part` of the skin's SVG (in the SVG's units) into `shown`: the whole document is
        // scaled and placed so that part lands there, and clipped to it.
        void DrawSvgPart(vg::VGContext& canvas, const Rectangle& shown, const Rectangle& part) const
        {
            const f32 sx = shown.width / part.width, sy = shown.height / part.height;
            canvas.PushClipRect(shown);
            foundation::vg::svg::SVGRenderer::Render(
                canvas, *Skin.Svg, Rectangle{shown.x - part.x * sx, shown.y - part.y * sy, Skin.Svg->width * sx, Skin.Svg->height * sy});
            canvas.PopClip();
        }

        // Draws an SVG layer as paths, sized and placed as an image of the SVG's own size would be,
        // with the part past the box clipped off.
        static void DrawSvgLayer(vg::VGContext& canvas, const BoxLayer& layer, const Rectangle& rect)
        {
            const f32 w = layer.Svg->width > 0.0f ? layer.Svg->width : rect.width;
            const f32 h = layer.Svg->height > 0.0f ? layer.Svg->height : rect.height;
            const Rectangle placed = PlaceImageLayer(layer, rect, w, h);
            if (placed.width <= 0.0f || placed.height <= 0.0f)
                return;
            const bool spills = placed.x < rect.x || placed.y < rect.y || placed.x + placed.width > rect.x + rect.width ||
                                placed.y + placed.height > rect.y + rect.height;
            if (spills)
                canvas.PushClipRect(rect);
            foundation::vg::svg::SVGRenderer::Render(canvas, *layer.Svg, placed);
            if (spills)
                canvas.PopClip();
        }

        void DrawLayer(vg::VGContext& canvas, const BoxLayer& layer, const Rectangle& rect) const
        {
            if (layer.IsImage)
            {
                if (layer.Image != nullptr)
                    DrawImageLayer(canvas, layer, rect);
                else if (layer.Svg != nullptr)
                    DrawSvgLayer(canvas, layer, rect);
                return;
            }
            const BoxGradient& g = layer.Gradient;
            if (g.Stops.Size() < 2)
                return;
            vg::PathBuilder shape;
            vg::ShapeBuilder::BuildRoundedRect(rect, Radii, shape);
            const core::Float2 center{rect.x + rect.width * 0.5f, rect.y + rect.height * 0.5f};
            // A repeating gradient repeats the span of its stops (as in CSS), while VG repeats its
            // whole line. So the line is cut to the stops' span (for linear, its ends; for radial,
            // its radius from the centre) and the stops are spread over it.
            const f32 first = g.Repeating ? g.Stops[0].offset : 0.0f;
            const f32 last = g.Repeating ? g.Stops[g.Stops.Size() - 1].offset : 1.0f;
            const bool repeating = g.Repeating && last - first > 1e-4f;
            const auto addStops = [&](auto& fill, f32 from, f32 to)
            {
                for (const vg::GradientStop& stop : g.Stops)
                    fill.AddStop(repeating ? (stop.offset - from) / (to - from) : stop.offset, stop.color);
                fill.spread = repeating ? vg::VGGradientSpread::Repeat : vg::VGGradientSpread::Pad;
            };
            if (g.Kind == BoxGradientKind::Linear)
            {
                // CSS's gradient line: through the centre at the angle (0 up, clockwise), as long as
                // the box's extent along it, so the corners take the end colours.
                const core::Float2 dir{Sin(g.Angle), -Cos(g.Angle)};
                const f32 half = (Abs(rect.width * dir.x) + Abs(rect.height * dir.y)) * 0.5f;
                const core::Float2 start = center - dir * half;
                const core::Float2 line = dir * (2.0f * half);
                vg::VGLinearGradientFill fill{start + line * first, start + line * last};
                addStops(fill, first, last);
                canvas.FillPath(shape.ToPath(), fill);
            }
            else if (g.Kind == BoxGradientKind::Radial)
            {
                // A circle at the centre reaching the farthest corner (CSS's default extent).
                const f32 radius = Sqrt(rect.width * rect.width + rect.height * rect.height) * 0.5f;
                // The radius ends at the last stop; the stops keep their distance from the centre.
                vg::VGRadialGradientFill fill{center, radius * last};
                addStops(fill, 0.0f, last);
                canvas.FillPath(shape.ToPath(), fill);
            }
            else
            {
                // CSS's conic 0 points up; VG's measures from +x, both clockwise on screen.
                vg::VGConicGradientFill fill{center, g.Angle - 1.57079632679f};
                for (const vg::GradientStop& stop : g.Stops)
                    fill.AddStop(stop.offset, stop.color);
                canvas.FillPath(shape.ToPath(), fill);
            }
        }
    };

    /// How a sheet styles one of a widget's parts (`slider::thumb`, `checkbox::box`): a box and an
    /// opacity, with each aspect applied only where the sheet sets it. A widget draws a part from its
    /// own look (a NodeBox with its colour, radii, outline and glow) and lays the sheet's aspects
    /// over it.
    struct PartStyle
    {
        NodeBox Box;
        f32 Opacity = 1.0f;
        bool HasBackground = false;  ///< background-color or background-image
        bool HasBorderWidth = false; ///< a border's width or style
        bool HasBorderColor = false;
        bool HasRadii = false;
        bool HasShadows = false;
        /// The part's `color`: the colour a text part draws in (a code editor's keyword, CSS's
        /// ::selection).
        Color TextColor{};
        bool HasColor = false;
        /// The part's `width` and `height` in pixels, where the sheet sets them. A part the widget
        /// sizes itself (a slider's track thickness, its thumb) uses them instead of its own.
        f32 Width = 0.0f;
        f32 Height = 0.0f;
        bool HasWidth = false;
        bool HasHeight = false;

        /// Returns `own` with the sheet's aspects laid over it.
        [[nodiscard]] NodeBox Over(NodeBox own) const
        {
            if (HasBackground)
            {
                own.BackgroundColor = Box.BackgroundColor;
                own.Layers = Box.Layers;
            }
            if (HasBorderWidth)
            {
                own.BorderWidths = Box.BorderWidths;
                own.BorderColors = Box.BorderColors;
            }
            else if (HasBorderColor)
                own.BorderColors = Box.BorderColors;
            if (HasRadii)
                own.Radii = Box.Radii;
            if (HasShadows)
                own.Shadows = Box.Shadows;
            return own;
        }
        /// Draws the part over `rect`: its own look with the sheet's aspects over it, at the sheet's
        /// opacity.
        void Draw(DrawContext& ctx, const Rect& rect, const NodeBox& own) const
        {
            if (Opacity <= 0.0f)
                return;
            if (Opacity < 1.0f)
                ctx.PushOpacity(Opacity);
            Over(own).DrawUnder(ctx, rect, false);
            if (Opacity < 1.0f)
                ctx.PopOpacity();
        }
    };

    /// Makes a part's own look: a fill with corner radii.
    [[nodiscard]] inline NodeBox FilledBox(Color color, vg::CornerRadii radii = {})
    {
        NodeBox box;
        box.BackgroundColor = color;
        box.Radii = radii;
        return box;
    }
    /// Makes a part's own look: an outline of `width` with corner radii.
    [[nodiscard]] inline NodeBox OutlinedBox(Color color, f32 width, vg::CornerRadii radii = {})
    {
        NodeBox box;
        box.BorderWidths = vg::BorderWidths(width);
        box.BorderColors = vg::BorderColors(color);
        box.Radii = radii;
        return box;
    }
}
