// Draconic::VG - :shapes partition.
//
// ShapeBuilder (common shapes as paths: rounded rect, circle/ellipse, regular
// polygon, star) and DashGenerator (dashed polylines from a pattern). Ported
// from Sedulous.VG/ShapeBuilder.bf and DashGenerator.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:shapes;

import draconic.foundation;
import :style;
import :path;

using namespace draconic::foundation;

export namespace draconic::vg
{
    /// Static helper for building common shapes as Paths.
    class ShapeBuilder
    {
    public:
        /// Build a rounded rectangle path with per-corner radii.
        static void BuildRoundedRect(Rectangle rect, CornerRadii radii, PathBuilder& builder)
        {
            const f32 maxRadius = Min(rect.width, rect.height) * 0.5f;
            const f32 tl = Min(radii.topLeft, maxRadius);
            const f32 tr = Min(radii.topRight, maxRadius);
            const f32 br = Min(radii.bottomRight, maxRadius);
            const f32 bl = Min(radii.bottomLeft, maxRadius);

            const f32 x = rect.x;
            const f32 y = rect.y;
            const f32 w = rect.width;
            const f32 h = rect.height;

            // Start at top edge, after top-left corner.
            builder.MoveTo(x + tl, y);

            // Top edge -> top-right corner.
            builder.LineTo(x + w - tr, y);
            if (tr > 0.0f)
                ArcCorner(builder, x + w - tr, y + tr, tr, -kHalfPi, 0.0f);

            // Right edge -> bottom-right corner.
            builder.LineTo(x + w, y + h - br);
            if (br > 0.0f)
                ArcCorner(builder, x + w - br, y + h - br, br, 0.0f, kHalfPi);

            // Bottom edge -> bottom-left corner.
            builder.LineTo(x + bl, y + h);
            if (bl > 0.0f)
                ArcCorner(builder, x + bl, y + h - bl, bl, kHalfPi, kPi);

            // Left edge -> top-left corner.
            builder.LineTo(x, y + tl);
            if (tl > 0.0f)
                ArcCorner(builder, x + tl, y + tl, tl, kPi, kPi * 1.5f);

            builder.Close();
        }

        /// Build a circle path using 4 cubic Bezier curves.
        static void BuildCircle(Float2 center, f32 radius, PathBuilder& builder)
        {
            BuildEllipse(center, radius, radius, builder);
        }

        /// Build an ellipse path using 4 cubic Bezier curves.
        static void BuildEllipse(Float2 center, f32 rx, f32 ry, PathBuilder& builder)
        {
            // Cubic Bezier approximation of quarter circle: control offset = radius * 0.5522847498.
            const f32 k = 0.5522847498f;
            const f32 kx = rx * k;
            const f32 ky = ry * k;

            const f32 cx = center.x;
            const f32 cy = center.y;

            builder.MoveTo(cx + rx, cy);
            builder.CubicTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
            builder.CubicTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
            builder.CubicTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
            builder.CubicTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
            builder.Close();
        }

        /// Build a regular polygon (e.g., hexagon with sides=6).
        static void BuildRegularPolygon(Float2 center, f32 radius, i32 sides, PathBuilder& builder)
        {
            if (sides < 3)
                return;

            const f32 angleStep = kTwoPi / static_cast<f32>(sides);
            // Start from top (-PI/2 rotation so a flat side is at bottom for even-sided polygons).
            const f32 startAngle = -kHalfPi;

            for (i32 i = 0; i < sides; ++i)
            {
                const f32 angle = startAngle + angleStep * static_cast<f32>(i);
                const f32 x = center.x + Cos(angle) * radius;
                const f32 y = center.y + Sin(angle) * radius;

                if (i == 0)
                    builder.MoveTo(x, y);
                else
                    builder.LineTo(x, y);
            }

            builder.Close();
        }

        /// Build a star shape.
        static void BuildStar(Float2 center, f32 outerRadius, f32 innerRadius, i32 points,
                              PathBuilder& builder)
        {
            if (points < 3)
                return;

            const i32 totalPoints = points * 2;
            const f32 angleStep = kTwoPi / static_cast<f32>(totalPoints);
            const f32 startAngle = -kHalfPi;

            for (i32 i = 0; i < totalPoints; ++i)
            {
                const f32 angle = startAngle + angleStep * static_cast<f32>(i);
                const f32 r = (i % 2 == 0) ? outerRadius : innerRadius;
                const f32 x = center.x + Cos(angle) * r;
                const f32 y = center.y + Sin(angle) * r;

                if (i == 0)
                    builder.MoveTo(x, y);
                else
                    builder.LineTo(x, y);
            }

            builder.Close();
        }

    private:
        /// Add a quarter-arc as a cubic Bezier approximation.
        static void ArcCorner(PathBuilder& builder, f32 cx, f32 cy, f32 r, f32 startAngle,
                              f32 endAngle)
        {
            const f32 sweep = endAngle - startAngle;
            const f32 alpha = Sin(sweep) *
                              (Sqrt(4.0f + 3.0f * Tan(sweep * 0.5f) * Tan(sweep * 0.5f)) - 1.0f) /
                              3.0f;

            const f32 cosStart = Cos(startAngle);
            const f32 sinStart = Sin(startAngle);
            const f32 cosEnd = Cos(endAngle);
            const f32 sinEnd = Sin(endAngle);

            const f32 x0 = cx + cosStart * r;
            const f32 y0 = cy + sinStart * r;
            const f32 x3 = cx + cosEnd * r;
            const f32 y3 = cy + sinEnd * r;

            const f32 dx0 = -sinStart * r;
            const f32 dy0 = cosStart * r;
            const f32 dx3 = -sinEnd * r;
            const f32 dy3 = cosEnd * r;

            builder.CubicTo(x0 + alpha * dx0, y0 + alpha * dy0, x3 - alpha * dx3, y3 - alpha * dy3,
                            x3, y3);
        }
    };

    /// Generates dashed polylines from a solid polyline and a dash pattern.
    class DashGenerator
    {
    public:
        /// Generate dashed segments from a polyline. Pattern alternates
        /// [dash, gap, dash, gap, ...]. Output is a list of polyline segments.
        static void GenerateDashes(Span<const Float2> points, bool closed, Span<const f32> pattern,
                                   f32 offset, Array<Array<Float2>>& output)
        {
            if (points.Size() < 2 || pattern.Size() == 0)
                return;

            // Calculate total pattern length.
            f32 patternLength = 0.0f;
            for (usize i = 0; i < pattern.Size(); ++i)
                patternLength += pattern[i];
            if (patternLength <= 0.0f)
                return;

            // Normalize offset into pattern range.
            f32 dashOffset = offset;
            while (dashOffset < 0.0f)
                dashOffset += patternLength;
            while (dashOffset >= patternLength)
                dashOffset -= patternLength;

            // Find starting position in pattern.
            usize patternIdx = 0;
            f32 patternRemaining = 0.0f;
            {
                f32 acc = 0.0f;
                for (usize i = 0; i < pattern.Size(); ++i)
                {
                    if (acc + pattern[i] > dashOffset)
                    {
                        patternIdx = i;
                        patternRemaining = pattern[i] - (dashOffset - acc);
                        break;
                    }
                    acc += pattern[i];
                }
            }

            bool isDash = (patternIdx % 2) == 0; // Even indices are dashes.
            i64 currentIdx = -1;                 // Index into output of the active segment, or -1.

            if (isDash)
            {
                output.PushBack(Array<Float2>());
                currentIdx = static_cast<i64>(output.Size()) - 1;
            }

            // Walk along the polyline.
            const usize totalPoints = closed ? points.Size() : points.Size() - 1;
            for (usize i = 0; i < totalPoints; ++i)
            {
                const Float2 p0 = points[i];
                const Float2 p1 = points[(i + 1) % points.Size()];

                Float2 edgeDir = p1 - p0;
                const f32 edgeLen = Length(edgeDir);
                if (edgeLen < 0.0001f)
                    continue;
                edgeDir = edgeDir / edgeLen;

                f32 edgeRemaining = edgeLen;
                Float2 currentPos = p0;

                while (edgeRemaining > 0.0001f)
                {
                    const f32 step = Min(edgeRemaining, patternRemaining);
                    const Float2 nextPos = currentPos + edgeDir * step;

                    if (isDash)
                    {
                        if (currentIdx < 0)
                        {
                            output.PushBack(Array<Float2>());
                            currentIdx = static_cast<i64>(output.Size()) - 1;
                        }
                        Array<Float2>& seg = output[static_cast<usize>(currentIdx)];
                        if (seg.IsEmpty())
                            seg.PushBack(currentPos);
                        seg.PushBack(nextPos);
                    }

                    edgeRemaining -= step;
                    patternRemaining -= step;
                    currentPos = nextPos;

                    if (patternRemaining <= 0.0001f)
                    {
                        // Advance to next pattern element.
                        patternIdx = (patternIdx + 1) % pattern.Size();
                        patternRemaining = pattern[patternIdx];
                        isDash = (patternIdx % 2) == 0;
                        currentIdx = -1; // A new dash will start a fresh segment.
                    }
                }
            }
        }
    };
}
