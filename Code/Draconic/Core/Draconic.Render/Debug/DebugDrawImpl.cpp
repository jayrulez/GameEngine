/// Draconic::Render - the `:debug_draw` partition (Debug layer).
///
/// Instance-based immediate-mode debug drawing, ported from SedulousEngine's Sedulous.Renderer.Debug
/// DebugDraw. Game code accumulates world-space lines/triangles/wireframes + screen/3D text over a
/// frame; the debug passes flush them. Each `DebugDraw` is one accumulator - the RenderSubsystem owns a
/// GLOBAL one (drawn in every view) + one per SCENE (drawn only when that scene renders), which is what
/// keeps side-by-side scenes from bleeding. Every 3D method takes `overlay`: false = depth-tested,
/// true = always-on-top. Immediate-mode: Clear() once per frame.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.render;

import draconic.foundation;

using namespace draconic::foundation;

namespace draconic::render::debug
{
    const Array<DebugVertex>& DebugDraw::OverlayLineVertices() const noexcept
    {
        return m_overlayLines;
    }

    const Array<DebugVertex>& DebugDraw::OverlayTriVertices() const noexcept
    {
        return m_overlayTris;
    }

    const Array<Debug3DTextCommand>& DebugDraw::TextCommands3D() const noexcept { return m_3dText; }

    bool DebugDraw::HasAnyDraws() const noexcept
    {
        return !m_lines.IsEmpty() || !m_overlayLines.IsEmpty() || !m_tris.IsEmpty() ||
               !m_overlayTris.IsEmpty() || !m_2d.IsEmpty() || !m_3dText.IsEmpty();
    }

    void DebugDraw::Clear() noexcept
    {
        m_lines.Clear();
        m_overlayLines.Clear();
        m_tris.Clear();
        m_overlayTris.Clear();
        m_2d.Clear();
        m_3dText.Clear();
        m_textChars.Clear();
    }

    void DebugDraw::DrawLine(Float3 from, Float3 to, Color color, bool overlay)
    {
        Array<DebugVertex>& list = overlay ? m_overlayLines : m_lines;
        const u32 c = PackColor(color);
        list.PushBack(DebugVertex{from, c});
        list.PushBack(DebugVertex{to, c});
    }

    void DebugDraw::DrawLineOverlay(Float3 from, Float3 to, Color color)
    {
        DrawLine(from, to, color, true);
    }

    void DebugDraw::DrawRay(Float3 origin, Float3 direction, Color color, bool overlay)
    {
        DrawLine(origin, origin + direction, color, overlay);
    }

    void DebugDraw::DrawTriangle(Float3 v0, Float3 v1, Float3 v2, Color color, bool overlay)
    {
        Array<DebugVertex>& list = overlay ? m_overlayTris : m_tris;
        const u32 c = PackColor(color);
        list.PushBack(DebugVertex{v0, c});
        list.PushBack(DebugVertex{v1, c});
        list.PushBack(DebugVertex{v2, c});
    }

    void DebugDraw::DrawQuad(Float3 v0, Float3 v1, Float3 v2, Float3 v3, Color color, bool overlay)
    {
        DrawTriangle(v0, v1, v2, color, overlay);
        DrawTriangle(v0, v2, v3, color, overlay);
    }

    void DebugDraw::DrawFilledBox(Float3 mn, Float3 mx, Color color, bool overlay)
    {
        const Float3 v0{mn.x, mn.y, mn.z}, v1{mx.x, mn.y, mn.z}, v2{mx.x, mn.y, mx.z},
            v3{mn.x, mn.y, mx.z};
        const Float3 v4{mn.x, mx.y, mn.z}, v5{mx.x, mx.y, mn.z}, v6{mx.x, mx.y, mx.z},
            v7{mn.x, mx.y, mx.z};
        DrawQuad(v0, v1, v2, v3, color, overlay); // bottom
        DrawQuad(v4, v7, v6, v5, color, overlay); // top
        DrawQuad(v0, v4, v5, v1, color, overlay); // front
        DrawQuad(v2, v6, v7, v3, color, overlay); // back
        DrawQuad(v0, v3, v7, v4, color, overlay); // left
        DrawQuad(v1, v5, v6, v2, color, overlay); // right
    }

    void DebugDraw::DrawFilledBoxCenter(Float3 center, Float3 halfExtents, Color color,
                                        bool overlay)
    {
        DrawFilledBox(center - halfExtents, center + halfExtents, color, overlay);
    }

    void DebugDraw::DrawWireBox(Float3 mn, Float3 mx, Color color, bool overlay)
    {
        const Float3 c000{mn.x, mn.y, mn.z}, c100{mx.x, mn.y, mn.z}, c010{mn.x, mx.y, mn.z},
            c110{mx.x, mx.y, mn.z};
        const Float3 c001{mn.x, mn.y, mx.z}, c101{mx.x, mn.y, mx.z}, c011{mn.x, mx.y, mx.z},
            c111{mx.x, mx.y, mx.z};
        DrawLine(c000, c100, color, overlay);
        DrawLine(c100, c101, color, overlay);
        DrawLine(c101, c001, color, overlay);
        DrawLine(c001, c000, color, overlay);
        DrawLine(c010, c110, color, overlay);
        DrawLine(c110, c111, color, overlay);
        DrawLine(c111, c011, color, overlay);
        DrawLine(c011, c010, color, overlay);
        DrawLine(c000, c010, color, overlay);
        DrawLine(c100, c110, color, overlay);
        DrawLine(c101, c111, color, overlay);
        DrawLine(c001, c011, color, overlay);
    }

    void DebugDraw::DrawWireBoxCenter(Float3 center, Float3 halfExtents, Color color, bool overlay)
    {
        DrawWireBox(center - halfExtents, center + halfExtents, color, overlay);
    }

    void DebugDraw::DrawTransformedBox(Float3 mn, Float3 mx, const Float4x4& world, Color color,
                                       bool overlay)
    {
        Float3 c[8];
        c[0] = TransformPoint(world, Float3{mn.x, mn.y, mn.z});
        c[1] = TransformPoint(world, Float3{mx.x, mn.y, mn.z});
        c[2] = TransformPoint(world, Float3{mn.x, mx.y, mn.z});
        c[3] = TransformPoint(world, Float3{mx.x, mx.y, mn.z});
        c[4] = TransformPoint(world, Float3{mn.x, mn.y, mx.z});
        c[5] = TransformPoint(world, Float3{mx.x, mn.y, mx.z});
        c[6] = TransformPoint(world, Float3{mn.x, mx.y, mx.z});
        c[7] = TransformPoint(world, Float3{mx.x, mx.y, mx.z});
        DrawLine(c[0], c[1], color, overlay);
        DrawLine(c[1], c[5], color, overlay);
        DrawLine(c[5], c[4], color, overlay);
        DrawLine(c[4], c[0], color, overlay);
        DrawLine(c[2], c[3], color, overlay);
        DrawLine(c[3], c[7], color, overlay);
        DrawLine(c[7], c[6], color, overlay);
        DrawLine(c[6], c[2], color, overlay);
        DrawLine(c[0], c[2], color, overlay);
        DrawLine(c[1], c[3], color, overlay);
        DrawLine(c[5], c[7], color, overlay);
        DrawLine(c[4], c[6], color, overlay);
    }

    void DebugDraw::DrawCircle(Float3 center, Float3 u, Float3 v, f32 radius, Color color,
                               i32 segments, bool overlay)
    {
        const Float3 uN = Normalized(u), vN = Normalized(v);
        Float3 prev = center + uN * radius;
        for (i32 i = 1; i <= segments; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(segments) * kPi * 2.0f;
            const Float3 point = center + uN * (radius * Cos(t)) + vN * (radius * Sin(t));
            DrawLine(prev, point, color, overlay);
            prev = point;
        }
    }

    void DebugDraw::DrawCircleNormal(Float3 center, f32 radius, Float3 normal, Color color,
                                     i32 segments, bool overlay)
    {
        const Float3 up = (Abs(normal.y) < 0.99f) ? Float3{0, 1, 0} : Float3{1, 0, 0};
        const Float3 right = Normalized(Cross(up, normal));
        const Float3 forward = Cross(normal, right);
        DrawCircle(center, right, forward, radius, color, segments, overlay);
    }

    void DebugDraw::DrawWireSphere(Float3 center, f32 radius, Color color, i32 segments,
                                   bool overlay)
    {
        DrawCircle(center, Float3{1, 0, 0}, Float3{0, 1, 0}, radius, color, segments, overlay);
        DrawCircle(center, Float3{0, 1, 0}, Float3{0, 0, 1}, radius, color, segments, overlay);
        DrawCircle(center, Float3{1, 0, 0}, Float3{0, 0, 1}, radius, color, segments, overlay);
    }

    void DebugDraw::DrawWireSphere(const BoundingSphere& sphere, Color color, i32 segments,
                                   bool overlay)
    {
        DrawWireSphere(sphere.center, sphere.radius, color, segments, overlay);
    }

    void DebugDraw::DrawWireSphereOverlay(Float3 center, f32 radius, Color color, i32 segments)
    {
        DrawWireSphere(center, radius, color, segments, true);
    }

    void DebugDraw::DrawCircleOverlay(Float3 center, Float3 u, Float3 v, f32 radius, Color color,
                                      i32 segments)
    {
        DrawCircle(center, u, v, radius, color, segments, true);
    }

    void DebugDraw::DrawCapsule(Float3 center, f32 radius, f32 height, Color color, i32 segments,
                                bool overlay)
    {
        const f32 halfHeight = height * 0.5f - radius;
        const Float3 top = center + Float3{0, halfHeight, 0},
                     bottom = center - Float3{0, halfHeight, 0};
        const f32 step = kPi * 2.0f / static_cast<f32>(segments);
        for (i32 i = 0; i < segments; ++i)
        { // vertical lines + the two end circles
            const f32 a0 = static_cast<f32>(i) * step, a1 = static_cast<f32>(i + 1) * step;
            const Float3 o0{Cos(a0) * radius, 0, Sin(a0) * radius},
                o1{Cos(a1) * radius, 0, Sin(a1) * radius};
            DrawLine(top + o0, bottom + o0, color, overlay);
            DrawLine(top + o0, top + o1, color, overlay);
            DrawLine(bottom + o0, bottom + o1, color, overlay);
        }
        const i32 halfSeg = segments / 2;
        const f32 halfStep = kPi / static_cast<f32>(halfSeg);
        for (i32 i = 0; i < halfSeg; ++i)
        { // hemisphere arcs (XY + ZY planes, top + bottom)
            const f32 a0 = static_cast<f32>(i) * halfStep, a1 = static_cast<f32>(i + 1) * halfStep;
            DrawLine(top + Float3{Sin(a0) * radius, Cos(a0) * radius, 0},
                     top + Float3{Sin(a1) * radius, Cos(a1) * radius, 0}, color, overlay);
            DrawLine(top + Float3{0, Cos(a0) * radius, Sin(a0) * radius},
                     top + Float3{0, Cos(a1) * radius, Sin(a1) * radius}, color, overlay);
            DrawLine(bottom + Float3{Sin(a0) * radius, -Cos(a0) * radius, 0},
                     bottom + Float3{Sin(a1) * radius, -Cos(a1) * radius, 0}, color, overlay);
            DrawLine(bottom + Float3{0, -Cos(a0) * radius, Sin(a0) * radius},
                     bottom + Float3{0, -Cos(a1) * radius, Sin(a1) * radius}, color, overlay);
        }
    }

    void DebugDraw::DrawAxis(const Float4x4& transform, f32 size, bool overlay)
    {
        const Float3 o{transform(3, 0), transform(3, 1), transform(3, 2)};
        const Float3 x{transform(0, 0), transform(0, 1), transform(0, 2)};
        const Float3 y{transform(1, 0), transform(1, 1), transform(1, 2)};
        const Float3 z{transform(2, 0), transform(2, 1), transform(2, 2)};
        DrawLine(o, o + x * size, Color{1, 0, 0, 1}, overlay);
        DrawLine(o, o + y * size, Color{0, 1, 0, 1}, overlay);
        DrawLine(o, o + z * size, Color{0, 0, 1, 1}, overlay);
    }

    void DebugDraw::DrawCross(Float3 center, f32 size, Color color, bool overlay)
    {
        const f32 h = size * 0.5f;
        DrawLine(center - Float3{h, 0, 0}, center + Float3{h, 0, 0}, color, overlay);
        DrawLine(center - Float3{0, h, 0}, center + Float3{0, h, 0}, color, overlay);
        DrawLine(center - Float3{0, 0, h}, center + Float3{0, 0, h}, color, overlay);
    }

    void DebugDraw::DrawArrow(Float3 start, Float3 end, Color color, f32 headSize, bool overlay)
    {
        DrawLine(start, end, color, overlay);
        const Float3 dir = Normalized(end - start);
        const Float3 perp1 =
            Normalized(Cross(dir, (Abs(dir.y) < 0.99f) ? Float3{0, 1, 0} : Float3{1, 0, 0}));
        const Float3 perp2 = Cross(dir, perp1);
        const Float3 headBase = end - dir * headSize;
        const f32 hr = headSize * 0.5f;
        DrawLine(end, headBase + perp1 * hr, color, overlay);
        DrawLine(end, headBase - perp1 * hr, color, overlay);
        DrawLine(end, headBase + perp2 * hr, color, overlay);
        DrawLine(end, headBase - perp2 * hr, color, overlay);
    }

    void DebugDraw::DrawGrid(Float3 center, f32 size, i32 divisions, Color color, bool overlay)
    {
        const f32 half = size * 0.5f, step = size / static_cast<f32>(divisions);
        for (i32 i = 0; i <= divisions; ++i)
        {
            const f32 t = static_cast<f32>(i) * step - half;
            DrawLine(center + Float3{-half, 0, t}, center + Float3{half, 0, t}, color, overlay);
            DrawLine(center + Float3{t, 0, -half}, center + Float3{t, 0, half}, color, overlay);
        }
    }

    void DebugDraw::DrawCylinder(Float3 center, f32 radius, f32 height, Color color, i32 segments,
                                 bool overlay)
    {
        const f32 hh = height * 0.5f, step = kPi * 2.0f / static_cast<f32>(segments);
        const Float3 top = center + Float3{0, hh, 0}, bottom = center - Float3{0, hh, 0};
        for (i32 i = 0; i < segments; ++i)
        {
            const f32 a0 = static_cast<f32>(i) * step, a1 = static_cast<f32>(i + 1) * step;
            const Float3 o0{Cos(a0) * radius, 0, Sin(a0) * radius},
                o1{Cos(a1) * radius, 0, Sin(a1) * radius};
            DrawLine(top + o0, bottom + o0, color, overlay);
            DrawLine(top + o0, top + o1, color, overlay);
            DrawLine(bottom + o0, bottom + o1, color, overlay);
        }
    }

    void DebugDraw::DrawCone(Float3 apex, Float3 direction, f32 length, f32 angle, Color color,
                             i32 segments, bool overlay)
    {
        const Float3 dir = Normalized(direction);
        const Float3 baseCenter = apex + dir * length;
        const f32 radius = length * (Sin(angle) / Cos(angle)); // tan(angle)
        const Float3 up = (Abs(dir.y) < 0.99f) ? Float3{0, 1, 0} : Float3{1, 0, 0};
        const Float3 right = Normalized(Cross(up, dir)), forward = Cross(dir, right);
        const f32 step = kPi * 2.0f / static_cast<f32>(segments);
        for (i32 i = 0; i < segments; ++i)
        {
            const f32 a0 = static_cast<f32>(i) * step, a1 = static_cast<f32>(i + 1) * step;
            const Float3 p0 = baseCenter + (right * Cos(a0) + forward * Sin(a0)) * radius;
            const Float3 p1 = baseCenter + (right * Cos(a1) + forward * Sin(a1)) * radius;
            DrawLine(p0, p1, color, overlay);
            DrawLine(apex, p0, color, overlay);
        }
    }

    void DebugDraw::DrawFrustum(const Float4x4& invViewProj, Color color, bool overlay)
    {
        Float3 corners[8];
        i32 idx = 0;
        for (i32 z = 0; z < 2; ++z)
            for (i32 y = 0; y < 2; ++y)
                for (i32 x = 0; x < 2; ++x)
                {
                    corners[idx++] = TransformPoint(
                        invViewProj,
                        Float3{x == 0 ? -1.0f : 1.0f, y == 0 ? -1.0f : 1.0f, static_cast<f32>(z)});
                }
        DrawLine(corners[0], corners[1], color, overlay);
        DrawLine(corners[1], corners[3], color, overlay);
        DrawLine(corners[3], corners[2], color, overlay);
        DrawLine(corners[2], corners[0], color, overlay);
        DrawLine(corners[4], corners[5], color, overlay);
        DrawLine(corners[5], corners[7], color, overlay);
        DrawLine(corners[7], corners[6], color, overlay);
        DrawLine(corners[6], corners[4], color, overlay);
        DrawLine(corners[0], corners[4], color, overlay);
        DrawLine(corners[1], corners[5], color, overlay);
        DrawLine(corners[2], corners[6], color, overlay);
        DrawLine(corners[3], corners[7], color, overlay);
    }

    void DebugDraw::DrawText3D(Float3 worldPos, StringView text, Color color)
    {
        if (text.IsEmpty())
        {
            return;
        }
        const i32 start = static_cast<i32>(m_textChars.Size());
        AppendChars(text);
        m_3dText.PushBack(
            Debug3DTextCommand{worldPos, color, start, static_cast<i32>(text.Size())});
    }

    void DebugDraw::DrawScreenText(f32 x, f32 y, StringView text, Color color, f32 scale)
    {
        if (text.IsEmpty())
        {
            return;
        }
        const i32 start = static_cast<i32>(m_textChars.Size());
        AppendChars(text);
        m_2d.PushBack(Debug2DCommand{Debug2DKind::Text, Float2{x, y}, Float2{0, 0}, color, start,
                                     static_cast<i32>(text.Size()), scale});
    }

    void DebugDraw::DrawScreenTextRight(f32 rightMargin, f32 y, StringView text, Color color,
                                        f32 scale)
    {
        if (text.IsEmpty())
        {
            return;
        }
        const i32 start = static_cast<i32>(m_textChars.Size());
        AppendChars(text);
        // negative x signals right-aligned (see the screen pass).
        m_2d.PushBack(Debug2DCommand{Debug2DKind::Text, Float2{-(rightMargin + 1.0f), y},
                                     Float2{0, 0}, color, start, static_cast<i32>(text.Size()),
                                     scale});
    }

    void DebugDraw::DrawScreenRect(f32 x, f32 y, f32 width, f32 height, Color color)
    {
        m_2d.PushBack(Debug2DCommand{Debug2DKind::Rectangle, Float2{x, y}, Float2{width, height},
                                     color, 0, 0, 1.0f});
    }

    void DebugDraw::AppendChars(StringView text)
    {
        const utf8char* d = text.Data();
        for (usize i = 0; i < text.Size(); ++i)
        {
            m_textChars.PushBack(static_cast<u8>(d[i]));
        }
    }
}
