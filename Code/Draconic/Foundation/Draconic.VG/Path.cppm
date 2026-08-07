// Draconic::VG - :path partition.
//
// The path data model: commands/points, segment iteration, the immutable Path
// (with bounds/contains/length/point-at-distance queries), the mutable
// PathBuilder, flattened sub-paths, and the curve->polyline PathFlattener.
// Ported from Sedulous.VG (PathCommand/PathSegment/PathIterator/Path/
// PathBuilder/FlattenedSubPath/PathFlattener).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:path;

import draconic.foundation;
import :enums;
import :curves;

using namespace draconic::foundation;

export namespace draconic::vg
{
    /// Commands that define path geometry.
    enum class PathCommand : u8
    {
        MoveTo,  ///< Move the pen to a new position (1 point).
        LineTo,  ///< Draw a straight line to a point (1 point).
        QuadTo,  ///< Quadratic Bezier (1 control point + 1 endpoint = 2 points).
        CubicTo, ///< Cubic Bezier (2 control points + 1 endpoint = 3 points).
        Close,   ///< Close the current sub-path back to the last MoveTo.
    };

    /// A single segment of a path, yielded during iteration.
    struct PathSegment
    {
        PathCommand command = PathCommand::MoveTo; ///< The command type for this segment.
        Span<const Float2> points; ///< Points for this command (not the start point).
        Float2 startPoint;         ///< The pen position before this segment.
    };

    /// Iterates over a Path, yielding PathSegment values.
    class PathIterator
    {
    public:
        PathIterator() = default;
        PathIterator(Span<const PathCommand> commands, Span<const Float2> points)
            : m_commands(commands), m_points(points)
        {
        }

        /// Get the next segment. Returns false when iteration is complete.
        bool GetNext(PathSegment& segment)
        {
            segment = PathSegment{};

            if (m_commandIndex >= m_commands.Size())
                return false;

            const PathCommand cmd = m_commands[m_commandIndex];
            segment.command = cmd;
            segment.startPoint = m_currentPoint;

            switch (cmd)
            {
            case PathCommand::MoveTo:
                segment.points = m_points.SubSpan(m_pointIndex, 1);
                m_currentPoint = m_points[m_pointIndex];
                m_subPathStart = m_currentPoint;
                m_pointIndex += 1;
                break;
            case PathCommand::LineTo:
                segment.points = m_points.SubSpan(m_pointIndex, 1);
                m_currentPoint = m_points[m_pointIndex];
                m_pointIndex += 1;
                break;
            case PathCommand::QuadTo:
                segment.points = m_points.SubSpan(m_pointIndex, 2);
                m_currentPoint = m_points[m_pointIndex + 1];
                m_pointIndex += 2;
                break;
            case PathCommand::CubicTo:
                segment.points = m_points.SubSpan(m_pointIndex, 3);
                m_currentPoint = m_points[m_pointIndex + 2];
                m_pointIndex += 3;
                break;
            case PathCommand::Close:
                segment.points = Span<const Float2>{};
                m_currentPoint = m_subPathStart;
                break;
            }

            ++m_commandIndex;
            return true;
        }

    private:
        Span<const PathCommand> m_commands;
        Span<const Float2> m_points;
        usize m_commandIndex = 0;
        usize m_pointIndex = 0;
        Float2 m_currentPoint;
        Float2 m_subPathStart;
    };

    /// A flattened sub-path consisting of line segments.
    class FlattenedSubPath
    {
    public:
        Array<Float2> points;  ///< The points forming this polyline.
        bool isClosed = false; ///< Whether this sub-path is closed.

        FlattenedSubPath() = default;
        explicit FlattenedSubPath(bool inIsClosed) : isClosed(inIsClosed) {}
    };

    /// An immutable vector path consisting of commands and points.
    /// Created via PathBuilder::ToPath().
    class Path
    {
    public:
        Path() = default;

        /// Create a path from pre-built command and point lists (takes ownership).
        Path(Array<PathCommand> commands, Array<Float2> points)
            : m_commands(Move(commands)), m_points(Move(points))
        {
        }

        /// The commands that define this path.
        [[nodiscard]] Span<const PathCommand> Commands() const
        {
            return Span<const PathCommand>(m_commands.Data(), m_commands.Size());
        }
        /// The points referenced by commands.
        [[nodiscard]] Span<const Float2> Points() const
        {
            return Span<const Float2>(m_points.Data(), m_points.Size());
        }

        [[nodiscard]] usize CommandCount() const { return m_commands.Size(); }
        [[nodiscard]] usize PointCount() const { return m_points.Size(); }

        /// Create an iterator over this path's segments.
        [[nodiscard]] PathIterator GetIterator() const
        {
            return PathIterator(Commands(), Points());
        }

        /// Count of sub-paths (number of MoveTo commands).
        [[nodiscard]] usize SubPathCount() const
        {
            usize count = 0;
            for (usize i = 0; i < m_commands.Size(); ++i)
                if (m_commands[i] == PathCommand::MoveTo)
                    ++count;
            return count;
        }

        /// Calculate the axis-aligned bounding box of this path.
        [[nodiscard]] Rectangle GetBounds() const
        {
            if (m_points.IsEmpty())
                return Rectangle{};

            f32 minX = m_points[0].x;
            f32 minY = m_points[0].y;
            f32 maxX = minX;
            f32 maxY = minY;

            for (usize i = 0; i < m_points.Size(); ++i)
            {
                const Float2 p = m_points[i];
                if (p.x < minX)
                    minX = p.x;
                if (p.y < minY)
                    minY = p.y;
                if (p.x > maxX)
                    maxX = p.x;
                if (p.y > maxY)
                    maxY = p.y;
            }

            return Rectangle{minX, minY, maxX - minX, maxY - minY};
        }

        /// Test whether a point is inside this path using the given fill rule.
        [[nodiscard]] bool Contains(Float2 point, FillRule fillRule) const
        {
            PathIterator iter = GetIterator();
            PathSegment seg;
            i32 windingNumber = 0;
            Float2 penPos = Float2::Zero;
            Float2 subPathStart = Float2::Zero;

            while (iter.GetNext(seg))
            {
                switch (seg.command)
                {
                case PathCommand::MoveTo:
                    penPos = seg.points[0];
                    subPathStart = penPos;
                    break;
                case PathCommand::LineTo:
                {
                    const Float2 endPt = seg.points[0];
                    windingNumber += RayCrossing(point, penPos, endPt);
                    penPos = endPt;
                    break;
                }
                case PathCommand::QuadTo:
                {
                    const Float2 cp = seg.points[0];
                    const Float2 endPt = seg.points[1];
                    const i32 steps = 8;
                    Float2 prev = penPos;
                    for (i32 i = 1; i <= steps; ++i)
                    {
                        const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                        const Float2 next = CurveUtils::QuadraticPointAt(penPos, cp, endPt, t);
                        windingNumber += RayCrossing(point, prev, next);
                        prev = next;
                    }
                    penPos = endPt;
                    break;
                }
                case PathCommand::CubicTo:
                {
                    const Float2 cp1 = seg.points[0];
                    const Float2 cp2 = seg.points[1];
                    const Float2 endPt = seg.points[2];
                    const i32 steps = 16;
                    Float2 prev = penPos;
                    for (i32 i = 1; i <= steps; ++i)
                    {
                        const f32 t = static_cast<f32>(i) / static_cast<f32>(steps);
                        const Float2 next = CurveUtils::CubicPointAt(penPos, cp1, cp2, endPt, t);
                        windingNumber += RayCrossing(point, prev, next);
                        prev = next;
                    }
                    penPos = endPt;
                    break;
                }
                case PathCommand::Close:
                    windingNumber += RayCrossing(point, penPos, subPathStart);
                    penPos = subPathStart;
                    break;
                }
            }

            switch (fillRule)
            {
            case FillRule::EvenOdd:
                return (windingNumber & 1) != 0;
            case FillRule::NonZero:
                return windingNumber != 0;
            }
            return false;
        }

        /// Get the total arc length of the path.
        [[nodiscard]] f32 GetLength() const
        {
            f32 totalLength = 0.0f;
            PathIterator iter = GetIterator();
            PathSegment seg;

            while (iter.GetNext(seg))
            {
                switch (seg.command)
                {
                case PathCommand::MoveTo:
                    break; // No length.
                case PathCommand::LineTo:
                    totalLength += Distance(seg.startPoint, seg.points[0]);
                    break;
                case PathCommand::QuadTo:
                    totalLength +=
                        CurveUtils::QuadraticLength(seg.startPoint, seg.points[0], seg.points[1]);
                    break;
                case PathCommand::CubicTo:
                    totalLength += CurveUtils::CubicLength(seg.startPoint, seg.points[0],
                                                           seg.points[1], seg.points[2]);
                    break;
                case PathCommand::Close:
                    break; // Close segment has implicit line.
                }
            }
            return totalLength;
        }

        /// Get the point at a given distance along the path.
        [[nodiscard]] Float2 GetPointAtDistance(f32 distance) const
        {
            f32 remaining = distance;
            PathIterator iter = GetIterator();
            PathSegment seg;

            while (iter.GetNext(seg))
            {
                f32 segLen = 0.0f;
                switch (seg.command)
                {
                case PathCommand::MoveTo:
                    continue;
                case PathCommand::LineTo:
                    segLen = Distance(seg.startPoint, seg.points[0]);
                    if (remaining <= segLen)
                        return Lerp(seg.startPoint, seg.points[0], remaining / segLen);
                    break;
                case PathCommand::QuadTo:
                    segLen =
                        CurveUtils::QuadraticLength(seg.startPoint, seg.points[0], seg.points[1]);
                    if (remaining <= segLen)
                        return CurveUtils::QuadraticPointAt(seg.startPoint, seg.points[0],
                                                            seg.points[1], remaining / segLen);
                    break;
                case PathCommand::CubicTo:
                    segLen = CurveUtils::CubicLength(seg.startPoint, seg.points[0], seg.points[1],
                                                     seg.points[2]);
                    if (remaining <= segLen)
                        return CurveUtils::CubicPointAt(seg.startPoint, seg.points[0],
                                                        seg.points[1], seg.points[2],
                                                        remaining / segLen);
                    break;
                case PathCommand::Close:
                    continue;
                }
                remaining -= segLen;
            }

            // Past end - return last point.
            if (!m_points.IsEmpty())
                return m_points[m_points.Size() - 1];
            return Float2::Zero;
        }

        /// Get the tangent direction at a given distance along the path.
        [[nodiscard]] Float2 GetTangentAtDistance(f32 distance) const
        {
            f32 remaining = distance;
            PathIterator iter = GetIterator();
            PathSegment seg;

            while (iter.GetNext(seg))
            {
                f32 segLen = 0.0f;
                switch (seg.command)
                {
                case PathCommand::MoveTo:
                    continue;
                case PathCommand::LineTo:
                    segLen = Distance(seg.startPoint, seg.points[0]);
                    if (remaining <= segLen)
                    {
                        const Float2 tangent = seg.points[0] - seg.startPoint;
                        const f32 len = Length(tangent);
                        if (len > 0.0001f)
                            return tangent / len;
                        return Float2{1.0f, 0.0f};
                    }
                    break;
                case PathCommand::QuadTo:
                    segLen =
                        CurveUtils::QuadraticLength(seg.startPoint, seg.points[0], seg.points[1]);
                    if (remaining <= segLen)
                        return CurveUtils::QuadraticTangentAt(seg.startPoint, seg.points[0],
                                                              seg.points[1], remaining / segLen);
                    break;
                case PathCommand::CubicTo:
                    segLen = CurveUtils::CubicLength(seg.startPoint, seg.points[0], seg.points[1],
                                                     seg.points[2]);
                    if (remaining <= segLen)
                        return CurveUtils::CubicTangentAt(seg.startPoint, seg.points[0],
                                                          seg.points[1], seg.points[2],
                                                          remaining / segLen);
                    break;
                case PathCommand::Close:
                    continue;
                }
                remaining -= segLen;
            }

            return Float2{1.0f, 0.0f};
        }

    private:
        /// Ray crossing test for point-in-polygon. +1/-1 for crossing direction, 0 for none.
        [[nodiscard]] static i32 RayCrossing(Float2 point, Float2 a, Float2 b)
        {
            if (a.y <= point.y)
            {
                if (b.y > point.y)
                {
                    // Upward crossing.
                    if (CrossProduct(b - a, point - a) > 0.0f)
                        return 1;
                }
            }
            else
            {
                if (b.y <= point.y)
                {
                    // Downward crossing.
                    if (CrossProduct(b - a, point - a) < 0.0f)
                        return -1;
                }
            }
            return 0;
        }

        [[nodiscard]] static f32 CrossProduct(Float2 a, Float2 b) { return a.x * b.y - a.y * b.x; }

        Array<PathCommand> m_commands;
        Array<Float2> m_points;
    };

    /// Mutable builder for constructing Path objects.
    class PathBuilder
    {
    public:
        /// Move the pen to a new position, starting a new sub-path.
        void MoveTo(f32 x, f32 y)
        {
            m_commands.PushBack(PathCommand::MoveTo);
            m_points.PushBack(Float2{x, y});
            m_currentPoint = Float2{x, y};
            m_subPathStart = m_currentPoint;
            m_hasMoveTo = true;
        }
        void MoveTo(Float2 point) { MoveTo(point.x, point.y); }

        /// Draw a straight line to the given point.
        void LineTo(f32 x, f32 y)
        {
            EnsureMoveTo();
            m_commands.PushBack(PathCommand::LineTo);
            m_points.PushBack(Float2{x, y});
            m_currentPoint = Float2{x, y};
        }
        void LineTo(Float2 point) { LineTo(point.x, point.y); }

        /// Draw a quadratic Bezier curve.
        void QuadTo(f32 cx, f32 cy, f32 x, f32 y)
        {
            EnsureMoveTo();
            m_commands.PushBack(PathCommand::QuadTo);
            m_points.PushBack(Float2{cx, cy});
            m_points.PushBack(Float2{x, y});
            m_currentPoint = Float2{x, y};
        }
        void QuadTo(Float2 control, Float2 end) { QuadTo(control.x, control.y, end.x, end.y); }

        /// Draw a cubic Bezier curve.
        void CubicTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y)
        {
            EnsureMoveTo();
            m_commands.PushBack(PathCommand::CubicTo);
            m_points.PushBack(Float2{c1x, c1y});
            m_points.PushBack(Float2{c2x, c2y});
            m_points.PushBack(Float2{x, y});
            m_currentPoint = Float2{x, y};
        }
        void CubicTo(Float2 control1, Float2 control2, Float2 end)
        {
            CubicTo(control1.x, control1.y, control2.x, control2.y, end.x, end.y);
        }

        /// Draw an SVG-style endpoint arc.
        void ArcTo(f32 rx, f32 ry, f32 xAxisRotation, bool largeArc, bool sweep, f32 x, f32 y)
        {
            EnsureMoveTo();
            const Float2 to{x, y};
            Array<Float2> cubicPoints;
            CurveUtils::ArcToCubics(m_currentPoint, rx, ry, xAxisRotation, largeArc, sweep, to,
                                    cubicPoints);

            // Each 3 points = one cubic segment (cp1, cp2, endpoint).
            for (usize i = 0; i + 2 < cubicPoints.Size(); i += 3)
            {
                m_commands.PushBack(PathCommand::CubicTo);
                m_points.PushBack(cubicPoints[i]);
                m_points.PushBack(cubicPoints[i + 1]);
                m_points.PushBack(cubicPoints[i + 2]);
            }

            m_currentPoint = to;
        }
        void ArcTo(f32 rx, f32 ry, f32 xAxisRotation, bool largeArc, bool sweep, Float2 to)
        {
            ArcTo(rx, ry, xAxisRotation, largeArc, sweep, to.x, to.y);
        }

        /// Close the current sub-path.
        void Close()
        {
            if (m_hasMoveTo)
            {
                m_commands.PushBack(PathCommand::Close);
                m_currentPoint = m_subPathStart;
            }
        }

        /// Build an immutable Path from the current state.
        [[nodiscard]] Path ToPath() const { return Path(m_commands, m_points); }

        /// Reset the builder for reuse.
        void Clear()
        {
            m_commands.Clear();
            m_points.Clear();
            m_currentPoint = Float2::Zero;
            m_subPathStart = Float2::Zero;
            m_hasMoveTo = false;
        }

        /// Current pen position.
        [[nodiscard]] Float2 CurrentPoint() const { return m_currentPoint; }
        /// Number of commands added so far.
        [[nodiscard]] usize CommandCount() const { return m_commands.Size(); }

    private:
        void EnsureMoveTo()
        {
            if (!m_hasMoveTo)
                MoveTo(0.0f, 0.0f);
        }

        Array<PathCommand> m_commands;
        Array<Float2> m_points;
        Float2 m_currentPoint;
        Float2 m_subPathStart;
        bool m_hasMoveTo = false;
    };

    /// Flattens a Path into polyline sub-paths by converting curves to line segments.
    class PathFlattener
    {
    public:
        /// Flatten a path into a list of polyline sub-paths.
        static void Flatten(const Path& path, f32 tolerance, Array<FlattenedSubPath>& output)
        {
            PathIterator iter = path.GetIterator();
            PathSegment seg;
            FlattenedSubPath* current = nullptr;

            while (iter.GetNext(seg))
            {
                switch (seg.command)
                {
                case PathCommand::MoveTo:
                    output.PushBack(FlattenedSubPath());
                    current = &output[output.Size() - 1];
                    current->points.PushBack(seg.points[0]);
                    break;
                case PathCommand::LineTo:
                    if (current != nullptr)
                        AddPoint(current->points, seg.points[0]);
                    break;
                case PathCommand::QuadTo:
                    if (current != nullptr)
                    {
                        const usize prevCount = current->points.Size();
                        CurveUtils::FlattenQuadratic(seg.startPoint, seg.points[0], seg.points[1],
                                                     tolerance, current->points);
                        DeduplicateFrom(current->points, prevCount);
                    }
                    break;
                case PathCommand::CubicTo:
                    if (current != nullptr)
                    {
                        const usize prevCount = current->points.Size();
                        CurveUtils::FlattenCubic(seg.startPoint, seg.points[0], seg.points[1],
                                                 seg.points[2], tolerance, current->points);
                        DeduplicateFrom(current->points, prevCount);
                    }
                    break;
                case PathCommand::Close:
                    if (current != nullptr)
                    {
                        current->isClosed = true;
                        // Remove trailing points coincident with the first point to avoid
                        // zero-length closing edges that produce degenerate normals.
                        if (current->points.Size() > 2)
                        {
                            const Float2 first = current->points[0];
                            while (current->points.Size() > 2 &&
                                   PointsEqual(current->points[current->points.Size() - 1], first))
                                current->points.RemoveAt(current->points.Size() - 1);
                        }
                    }
                    break;
                }
            }
        }

    private:
        static constexpr f32 DistTol = 0.01f;

        /// Add a point only if it's not coincident with the last point.
        static void AddPoint(Array<Float2>& points, Float2 p)
        {
            if (!points.IsEmpty() && PointsEqual(points[points.Size() - 1], p))
                return;
            points.PushBack(p);
        }

        /// Remove any newly-added points that are coincident with their predecessor.
        static void DeduplicateFrom(Array<Float2>& points, usize startIdx)
        {
            if (startIdx == 0)
                startIdx = 1;
            usize i = startIdx;
            while (i < points.Size())
            {
                if (PointsEqual(points[i], points[i - 1]))
                    points.RemoveAt(i);
                else
                    ++i;
            }
        }

        /// Check if two points are within distance tolerance.
        [[nodiscard]] static bool PointsEqual(Float2 a, Float2 b)
        {
            const f32 dx = b.x - a.x;
            const f32 dy = b.y - a.y;
            return dx * dx + dy * dy < DistTol * DistTol;
        }
    };
}
