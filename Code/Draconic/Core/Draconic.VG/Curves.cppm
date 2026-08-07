// Draconic::VG - :curves partition.
//
// Bezier curve math: point/tangent evaluation, arc-length approximation,
// adaptive flattening to polylines, and SVG endpoint-arc -> cubic conversion.
// Ported from Sedulous.VG/CurveUtils.bf.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:curves;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::vg
{
    /// Utility functions for Bezier curve math.
    class CurveUtils
    {
    public:
        /// Evaluate a point on a quadratic Bezier curve at parameter t.
        [[nodiscard]] static Float2 QuadraticPointAt(Float2 p0, Float2 p1, Float2 p2, f32 t)
        {
            const f32 mt = 1.0f - t;
            return p0 * (mt * mt) + p1 * (2.0f * mt * t) + p2 * (t * t);
        }

        /// Evaluate a point on a cubic Bezier curve at parameter t.
        [[nodiscard]] static Float2 CubicPointAt(Float2 p0, Float2 p1, Float2 p2, Float2 p3, f32 t)
        {
            const f32 mt = 1.0f - t;
            const f32 mt2 = mt * mt;
            const f32 t2 = t * t;
            return p0 * (mt2 * mt) + p1 * (3.0f * mt2 * t) + p2 * (3.0f * mt * t2) + p3 * (t2 * t);
        }

        /// Get the tangent (normalized direction) of a quadratic Bezier at parameter t.
        [[nodiscard]] static Float2 QuadraticTangentAt(Float2 p0, Float2 p1, Float2 p2, f32 t)
        {
            const f32 mt = 1.0f - t;
            const Float2 tangent = (p1 - p0) * (2.0f * mt) + (p2 - p1) * (2.0f * t);
            const f32 len = Length(tangent);
            if (len > 0.0001f)
                return tangent / len;
            return Float2{1.0f, 0.0f};
        }

        /// Get the tangent (normalized direction) of a cubic Bezier at parameter t.
        [[nodiscard]] static Float2 CubicTangentAt(Float2 p0, Float2 p1, Float2 p2, Float2 p3,
                                                   f32 t)
        {
            const f32 mt = 1.0f - t;
            const f32 mt2 = mt * mt;
            const f32 t2 = t * t;
            const Float2 tangent =
                (p1 - p0) * (3.0f * mt2) + (p2 - p1) * (6.0f * mt * t) + (p3 - p2) * (3.0f * t2);
            const f32 len = Length(tangent);
            if (len > 0.0001f)
                return tangent / len;
            return Float2{1.0f, 0.0f};
        }

        /// Approximate the arc length of a quadratic Bezier using subdivision.
        [[nodiscard]] static f32 QuadraticLength(Float2 p0, Float2 p1, Float2 p2, i32 steps = 16)
        {
            f32 length = 0.0f;
            Float2 prev = p0;
            for (i32 i = 1; i <= steps; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                const Float2 next = QuadraticPointAt(p0, p1, p2, t);
                length += Distance(prev, next);
                prev = next;
            }
            return length;
        }

        /// Approximate the arc length of a cubic Bezier using subdivision.
        [[nodiscard]] static f32 CubicLength(Float2 p0, Float2 p1, Float2 p2, Float2 p3,
                                             i32 steps = 16)
        {
            f32 length = 0.0f;
            Float2 prev = p0;
            for (i32 i = 1; i <= steps; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                const Float2 next = CubicPointAt(p0, p1, p2, p3, t);
                length += Distance(prev, next);
                prev = next;
            }
            return length;
        }

        /// Flatten a quadratic Bezier into line segments using adaptive subdivision.
        static void FlattenQuadratic(Float2 p0, Float2 p1, Float2 p2, f32 tolerance,
                                     Array<Float2>& output)
        {
            FlattenQuadraticRecursive(p0, p1, p2, tolerance * tolerance, 0, output);
            output.PushBack(p2);
        }

        /// Flatten a cubic Bezier into line segments using adaptive subdivision.
        static void FlattenCubic(Float2 p0, Float2 p1, Float2 p2, Float2 p3, f32 tolerance,
                                 Array<Float2>& output)
        {
            FlattenCubicRecursive(p0, p1, p2, p3, tolerance * tolerance, 0, output);
            output.PushBack(p3);
        }

        /// Convert an SVG endpoint arc to cubic Bezier curves (groups of 3 points: cp1, cp2, end).
        static void ArcToCubics(Float2 from, f32 rx, f32 ry, f32 xAxisRotation, bool largeArc,
                                bool sweep, Float2 to, Array<Float2>& controlPoints)
        {
            // Handle degenerate cases.
            if (Distance(from, to) < 0.0001f)
                return;

            rx = Abs(rx);
            ry = Abs(ry);
            if (rx < 0.0001f || ry < 0.0001f)
            {
                // Degenerate to line.
                controlPoints.PushBack(from);
                controlPoints.PushBack(to);
                controlPoints.PushBack(to);
                return;
            }

            const f32 sinPhi = Sin(xAxisRotation);
            const f32 cosPhi = Cos(xAxisRotation);

            // Step 1: Compute (x1', y1') - transform to unit circle space.
            const f32 dx = (from.x - to.x) * 0.5f;
            const f32 dy = (from.y - to.y) * 0.5f;
            const f32 x1p = cosPhi * dx + sinPhi * dy;
            const f32 y1p = -sinPhi * dx + cosPhi * dy;

            // Step 2: Compute (cx', cy') - center in transformed space.
            const f32 x1p2 = x1p * x1p;
            const f32 y1p2 = y1p * y1p;
            f32 rx2 = rx * rx;
            f32 ry2 = ry * ry;

            // Scale radii if needed.
            const f32 lambda = x1p2 / rx2 + y1p2 / ry2;
            if (lambda > 1.0f)
            {
                const f32 sqrtLambda = Sqrt(lambda);
                rx *= sqrtLambda;
                ry *= sqrtLambda;
            }

            rx2 = rx * rx;
            ry2 = ry * ry;

            f32 sq = (rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2) / (rx2 * y1p2 + ry2 * x1p2);
            if (sq < 0.0f)
                sq = 0.0f;
            f32 coeff = Sqrt(sq);
            if (largeArc == sweep)
                coeff = -coeff;

            const f32 cxp = coeff * rx * y1p / ry;
            const f32 cyp = coeff * -ry * x1p / rx;

            // Step 3: Compute (cx, cy) from (cx', cy').
            const f32 mx = (from.x + to.x) * 0.5f;
            const f32 my = (from.y + to.y) * 0.5f;
            const f32 cx = cosPhi * cxp - sinPhi * cyp + mx;
            const f32 cy = sinPhi * cxp + cosPhi * cyp + my;

            // Step 4: Compute angles.
            const f32 startAngle = VectorAngle(1.0f, 0.0f, (x1p - cxp) / rx, (y1p - cyp) / ry);
            f32 deltaAngle = VectorAngle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx,
                                         (-y1p - cyp) / ry);

            if (!sweep && deltaAngle > 0.0f)
                deltaAngle -= kTwoPi;
            else if (sweep && deltaAngle < 0.0f)
                deltaAngle += kTwoPi;

            // Step 5: Convert arc to cubic Bezier segments.
            const i32 segments = Max(1, static_cast<i32>(Abs(deltaAngle) / kHalfPi + 0.999f));
            const f32 segAngle = deltaAngle / static_cast<f32>(segments);

            for (i32 i = 0; i < segments; ++i)
            {
                const f32 a1 = startAngle + segAngle * static_cast<f32>(i);
                const f32 a2 = startAngle + segAngle * static_cast<f32>(i + 1);
                ArcSegmentToCubic(cx, cy, rx, ry, xAxisRotation, a1, a2, controlPoints);
            }
        }

    private:
        static void FlattenQuadraticRecursive(Float2 p0, Float2 p1, Float2 p2, f32 toleranceSq,
                                              i32 depth, Array<Float2>& output)
        {
            if (depth > 16)
            {
                output.PushBack(p0);
                return;
            }

            // Check flatness: distance from control point to line p0-p2.
            const Float2 mid = (p0 + p2) * 0.5f;
            const Float2 deviation = p1 - mid;
            if (deviation.x * deviation.x + deviation.y * deviation.y <= toleranceSq)
            {
                output.PushBack(p0);
                return;
            }

            // Subdivide.
            const Float2 p01 = (p0 + p1) * 0.5f;
            const Float2 p12 = (p1 + p2) * 0.5f;
            const Float2 p012 = (p01 + p12) * 0.5f;

            FlattenQuadraticRecursive(p0, p01, p012, toleranceSq, depth + 1, output);
            FlattenQuadraticRecursive(p012, p12, p2, toleranceSq, depth + 1, output);
        }

        static void FlattenCubicRecursive(Float2 p0, Float2 p1, Float2 p2, Float2 p3,
                                          f32 toleranceSq, i32 depth, Array<Float2>& output)
        {
            if (depth > 16)
            {
                output.PushBack(p0);
                return;
            }

            // Check flatness: max distance of control points from chord.
            const f32 d1 = PointToLineDistanceSq(p1, p0, p3);
            const f32 d2 = PointToLineDistanceSq(p2, p0, p3);
            if (d1 <= toleranceSq && d2 <= toleranceSq)
            {
                output.PushBack(p0);
                return;
            }

            // De Casteljau subdivision at t=0.5.
            const Float2 p01 = (p0 + p1) * 0.5f;
            const Float2 p12 = (p1 + p2) * 0.5f;
            const Float2 p23 = (p2 + p3) * 0.5f;
            const Float2 p012 = (p01 + p12) * 0.5f;
            const Float2 p123 = (p12 + p23) * 0.5f;
            const Float2 p0123 = (p012 + p123) * 0.5f;

            FlattenCubicRecursive(p0, p01, p012, p0123, toleranceSq, depth + 1, output);
            FlattenCubicRecursive(p0123, p123, p23, p3, toleranceSq, depth + 1, output);
        }

        [[nodiscard]] static f32 PointToLineDistanceSq(Float2 point, Float2 lineStart,
                                                       Float2 lineEnd)
        {
            const f32 dx = lineEnd.x - lineStart.x;
            const f32 dy = lineEnd.y - lineStart.y;
            const f32 lenSq = dx * dx + dy * dy;
            if (lenSq < 0.0001f)
                return DistanceSquared(point, lineStart);

            const f32 cross = (point.x - lineStart.x) * dy - (point.y - lineStart.y) * dx;
            return (cross * cross) / lenSq;
        }

        [[nodiscard]] static f32 VectorAngle(f32 ux, f32 uy, f32 vx, f32 vy)
        {
            const f32 dot = ux * vx + uy * vy;
            const f32 cross = ux * vy - uy * vx;
            return Atan2(cross, dot);
        }

        static void ArcSegmentToCubic(f32 cx, f32 cy, f32 rx, f32 ry, f32 phi, f32 a1, f32 a2,
                                      Array<Float2>& controlPoints)
        {
            const f32 alpha =
                Sin(a2 - a1) *
                (Sqrt(4.0f + 3.0f * Tan((a2 - a1) * 0.5f) * Tan((a2 - a1) * 0.5f)) - 1.0f) / 3.0f;

            const f32 sinPhi = Sin(phi);
            const f32 cosPhi = Cos(phi);

            const f32 cosA1 = Cos(a1);
            const f32 sinA1 = Sin(a1);
            const f32 cosA2 = Cos(a2);
            const f32 sinA2 = Sin(a2);

            // Start point.
            const f32 x1 = cosPhi * rx * cosA1 - sinPhi * ry * sinA1 + cx;
            const f32 y1 = sinPhi * rx * cosA1 + cosPhi * ry * sinA1 + cy;

            // End point.
            const f32 x4 = cosPhi * rx * cosA2 - sinPhi * ry * sinA2 + cx;
            const f32 y4 = sinPhi * rx * cosA2 + cosPhi * ry * sinA2 + cy;

            // Control point 1.
            const f32 dx1 = -cosPhi * rx * sinA1 - sinPhi * ry * cosA1;
            const f32 dy1 = -sinPhi * rx * sinA1 + cosPhi * ry * cosA1;

            // Control point 2.
            const f32 dx2 = -cosPhi * rx * sinA2 - sinPhi * ry * cosA2;
            const f32 dy2 = -sinPhi * rx * sinA2 + cosPhi * ry * cosA2;

            controlPoints.PushBack(Float2{x1 + alpha * dx1, y1 + alpha * dy1}); // CP1
            controlPoints.PushBack(Float2{x4 - alpha * dx2, y4 - alpha * dy2}); // CP2
            controlPoints.PushBack(Float2{x4, y4});                             // End point
        }
    };
}
