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

export module draconic.render:debug_draw;

import draconic.foundation;

using namespace draconic::foundation;

export namespace draconic::render::debug
{

    // World-space geometry vertex (16B): position + packed RGBA8 color (byte0=R -> matches Unorm8x4).
    struct DebugVertex
    {
        Float3 position;
        u32 color = 0xFFFFFFFFu;
    };

    // Screen/text vertex (24B): pixel-or-world position + font-atlas uv + packed color.
    struct DebugTextVertex
    {
        Float3 position;
        Float2 uv;
        u32 color = 0xFFFFFFFFu;
    };

    enum class Debug2DKind : u8
    {
        Text,
        Rectangle
    };

    // One 2D overlay command (pixel-space text or filled rect). Text glyphs live in the list's char store.
    struct Debug2DCommand
    {
        Debug2DKind kind = Debug2DKind::Text;
        Float2
            position{}; // pixels, top-left origin (negative x = right-aligned, see DrawScreenTextRight)
        Float2 size{}; // pixels (rects)
        Color color{};
        i32 textStart = 0;
        i32 textLength = 0;
        f32 scale = 1.0f;
    };

    // One 3D text command (text anchored at a world position, projected to screen at render).
    struct Debug3DTextCommand
    {
        Float3 worldPos{};
        Color color{};
        i32 textStart = 0;
        i32 textLength = 0;
    };

    // Pack a float Color to RGBA8 with R in the low byte (so an Unorm8x4 vertex attribute reads R,G,B,A).
    [[nodiscard]] inline u32 PackColor(const Color& c) noexcept
    {
        const auto b8 = [](f32 x) -> u32
        { return static_cast<u32>(Clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return b8(c.r) | (b8(c.g) << 8) | (b8(c.b) << 16) | (b8(c.a) << 24);
    }

    // Transform a point by a row-vector row-major matrix (mul(float4(p,1), m)) with perspective divide.
    [[nodiscard]] inline Float3 TransformPoint(const Float4x4& m, Float3 p) noexcept
    {
        const f32 x = p.x * m(0, 0) + p.y * m(1, 0) + p.z * m(2, 0) + m(3, 0);
        const f32 y = p.x * m(0, 1) + p.y * m(1, 1) + p.z * m(2, 1) + m(3, 1);
        const f32 z = p.x * m(0, 2) + p.y * m(1, 2) + p.z * m(2, 2) + m(3, 2);
        const f32 w = p.x * m(0, 3) + p.y * m(1, 3) + p.z * m(2, 3) + m(3, 3);
        const f32 iw = (w != 0.0f) ? 1.0f / w : 1.0f;
        return Float3{x * iw, y * iw, z * iw};
    }

    class DebugDraw
    {
    public:
        // --- read-only accessors for the passes ---
        [[nodiscard]] const Array<DebugVertex>& LineVertices() const noexcept { return m_lines; }
        [[nodiscard]] const Array<DebugVertex>& OverlayLineVertices() const noexcept;
        [[nodiscard]] const Array<DebugVertex>& TriVertices() const noexcept { return m_tris; }
        [[nodiscard]] const Array<DebugVertex>& OverlayTriVertices() const noexcept;
        [[nodiscard]] const Array<Debug2DCommand>& Commands2D() const noexcept { return m_2d; }
        [[nodiscard]] const Array<Debug3DTextCommand>& TextCommands3D() const noexcept;
        [[nodiscard]] const Array<u8>& TextChars() const noexcept { return m_textChars; }

        [[nodiscard]] bool HasAnyDraws() const noexcept;

        // Clears all accumulated draws. Called by the renderer once per frame.
        void Clear() noexcept;

        // ==================== Lines ====================
        void DrawLine(Float3 from, Float3 to, Color color, bool overlay = false);
        void DrawLineOverlay(Float3 from, Float3 to, Color color);
        void DrawRay(Float3 origin, Float3 direction, Color color, bool overlay = false);

        // ==================== Filled ====================
        void DrawTriangle(Float3 v0, Float3 v1, Float3 v2, Color color, bool overlay = false);
        void DrawQuad(Float3 v0, Float3 v1, Float3 v2, Float3 v3, Color color,
                      bool overlay = false);
        void DrawFilledBox(Float3 mn, Float3 mx, Color color, bool overlay = false);
        void DrawFilledBoxCenter(Float3 center, Float3 halfExtents, Color color,
                                 bool overlay = false);

        // ==================== Wireframe ====================
        void DrawWireBox(Float3 mn, Float3 mx, Color color, bool overlay = false);
        void DrawWireBoxCenter(Float3 center, Float3 halfExtents, Color color,
                               bool overlay = false);
        // Local AABB transformed by a world matrix (OBB).
        void DrawTransformedBox(Float3 mn, Float3 mx, const Float4x4& world, Color color,
                                bool overlay = false);
        void DrawCircle(Float3 center, Float3 u, Float3 v, f32 radius, Color color,
                        i32 segments = 32, bool overlay = false);
        void DrawCircleNormal(Float3 center, f32 radius, Float3 normal, Color color,
                              i32 segments = 32, bool overlay = false);
        void DrawWireSphere(Float3 center, f32 radius, Color color, i32 segments = 24,
                            bool overlay = false);
        void DrawWireSphere(const BoundingSphere& sphere, Color color, i32 segments = 24,
                            bool overlay = false);
        void DrawWireSphereOverlay(Float3 center, f32 radius, Color color, i32 segments = 24);
        void DrawCircleOverlay(Float3 center, Float3 u, Float3 v, f32 radius, Color color,
                               i32 segments = 32);
        // Wireframe capsule (cylinder body + two hemisphere caps).
        void DrawCapsule(Float3 center, f32 radius, f32 height, Color color, i32 segments = 16,
                         bool overlay = false);
        // World basis axes of a transform (red=X, green=Y, blue=Z). Rows are the basis (row-vector convention).
        void DrawAxis(const Float4x4& transform, f32 size = 1.0f, bool overlay = false);
        void DrawCross(Float3 center, f32 size, Color color, bool overlay = false);
        void DrawArrow(Float3 start, Float3 end, Color color, f32 headSize = 0.1f,
                       bool overlay = false);
        void DrawGrid(Float3 center, f32 size, i32 divisions, Color color, bool overlay = false);
        void DrawCylinder(Float3 center, f32 radius, f32 height, Color color, i32 segments = 16,
                          bool overlay = false);
        void DrawCone(Float3 apex, Float3 direction, f32 length, f32 angle, Color color,
                      i32 segments = 16, bool overlay = false);
        // Camera frustum edges from an inverse view-projection (NDC z in [0,1]).
        void DrawFrustum(const Float4x4& invViewProj, Color color, bool overlay = false);

        // ==================== Text + 2D ====================
        void DrawText3D(Float3 worldPos, StringView text, Color color);
        void DrawScreenText(f32 x, f32 y, StringView text, Color color, f32 scale = 1.0f);
        void DrawScreenTextRight(f32 rightMargin, f32 y, StringView text, Color color,
                                 f32 scale = 1.0f);
        void DrawScreenRect(f32 x, f32 y, f32 width, f32 height, Color color);

    private:
        void AppendChars(StringView text);

        Array<DebugVertex> m_lines;
        Array<DebugVertex> m_overlayLines;
        Array<DebugVertex> m_tris;
        Array<DebugVertex> m_overlayTris;
        Array<Debug2DCommand> m_2d;
        Array<Debug3DTextCommand> m_3dText;
        Array<u8> m_textChars;
    };

} // namespace draconic::render::debug
