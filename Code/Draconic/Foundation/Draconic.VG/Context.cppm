// Draconic::VG - :context partition.
//
// VGState (the per-state-stack snapshot) and VGContext - the main user-facing
// vector-graphics API. Immediate-mode drawing of paths, shapes, images, 9-slice,
// and text; produces a batched VGBatch for an external renderer to consume.
// Ported from Sedulous.VG (VGState/VGContext). The 2D transform is a Float4x4
// (mirroring Sedulous's 4x4 Matrix); colors are float Color (packed to Color32 only at the vertex).

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.vg:context;

import draconic.foundation;
import draconic.image;
import draconic.fonts;
import :enums;
import :vertex;
import :style;
import :fills;
import :path;
import :shapes;
import :tessellation;
import :batch;

using namespace draconic::foundation;

export namespace draconic::vg
{
    namespace image = draconic::image;
    namespace fonts = draconic::fonts;

    /// Internal per-state-stack snapshot for VGContext.
    struct VGState
    {
        Float4x4 transform = Float4x4::Identity();
        Rectangle clipRect{};
        VGClipMode clipMode = VGClipMode::None;
        f32 opacity = 1.0f;
        i32 stencilRef = 0;
    };

    /// Main user-facing API for vector graphics drawing. Produces batched
    /// geometry (VGBatch) for rendering.
    class VGContext
    {
    public:
        /// Distinct gradient ramps kept in the persistent LUT cache before a whole-cache
        /// eviction (announced via VGBatch::evictedTextures). Generous: a UI with a few
        /// static gradients never gets near it; only continuously ANIMATED ramp colors do.
        static constexpr usize kMaxGradientLutCacheEntries = 256;

        explicit VGContext(fonts::IFontService* fontService = nullptr) : m_fontService(fontService)
        {
            m_stateStack.Reserve(16);

            // 1x1 white texture for solid-color draws (Textures[0] -> color passthrough).
            const u8 whitePixel[4] = {255, 255, 255, 255};
            m_whiteTexture = image::OwnedImageData(1, 1, image::PixelFormat::RGBA8,
                                                   Span<const u8>(whitePixel, 4));
            m_batch.textures.PushBack(&m_whiteTexture);
        }

        /// The font service used by the convenience CachedFont text overloads.
        [[nodiscard]] fonts::IFontService* FontService() const { return m_fontService; }

        // === Output ===

        /// Get the current batch for rendering.
        [[nodiscard]] VGBatch& GetBatch()
        {
            FlushCurrentCommand();
            return m_batch;
        }

        /// Clear all content and reset state.
        void Clear()
        {
            m_batch.Clear();
            m_stateStack.Clear();
            m_clipStack.Clear();
            m_opacityStack.Clear();
            m_currentState = VGState{};
            m_currentBlendMode = VGBlendMode::Normal;
            m_currentDrawMode = VGDrawMode::Default;
            m_currentGradientSpread = VGGradientSpread::Pad;
            m_currentTextureIndex = 0;
            m_commandStartIndex = 0;

            // Gradient LUTs live in a PERSISTENT content-keyed cache (see BindGradientLut):
            // stable ImageData pointers per distinct ramp, so the renderer's identity-keyed
            // GPU texture cache stays valid across frames. Last frame's eviction list has
            // been consumed by now - the held images can finally die.
            m_evictedLutHold.Clear();
            if (m_gradientLutCache.Size() > kMaxGradientLutCacheEntries)
            {
                // Over budget (many DISTINCT ramps - e.g. animated gradient colors): drop
                // the whole cache and tell the renderer which sources died via the batch's
                // eviction list (identity keys only - never dereferenced). The images are
                // held one frame so those keys are not dangling while in flight.
                for (auto& kv : m_gradientLutCache)
                {
                    m_batch.evictedTextures.PushBack(kv.value.Get());
                    m_evictedLutHold.PushBack(Move(kv.value));
                }
                m_gradientLutCache.Clear();
            }

            // Re-add the white texture at index 0 for solid color drawing.
            m_batch.textures.PushBack(&m_whiteTexture);
        }

        /// Tessellation tolerance (lower = smoother curves, more vertices).
        [[nodiscard]] f32 Tolerance() const { return m_tolerance; }
        void SetTolerance(f32 tolerance) { m_tolerance = tolerance; }

        /// Enable per-pixel radial/conic gradients. A host must ONLY enable this if its VGRenderer
        /// was given the vg_grad_radial/vg_grad_conic shaders; otherwise those draw modes have no
        /// pipeline. Off (default): radial/conic use the affine LUT approximation, which every
        /// renderer can draw. Linear gradients are exact either way.
        void SetPerPixelGradients(bool enabled) { m_perPixelGradients = enabled; }
        [[nodiscard]] bool PerPixelGradients() const { return m_perPixelGradients; }

        /// Enable stencil-then-cover for COMPLEX path fills (holes, self-intersection,
        /// even-odd) - the fill-correctness path. Opt-in by the HOST, and only after it
        /// gave the renderer a stencil attachment (VGRenderer target config): the write/
        /// cover commands are dropped by a renderer without stencil pipelines. Convex
        /// single-contour fills keep the tessellated fast path either way.
        void SetStencilFills(bool enabled) { m_stencilFills = enabled; }
        [[nodiscard]] bool StencilFills() const { return m_stencilFills; }

        // === Path clipping (stencil bit 0x80; see VGFillPhase) ===

        /// Clip subsequent draws to `path` (NonZero winding; EvenOdd clip paths are not
        /// supported in v1). Requires stencil fills (a host-provided stencil attachment);
        /// without one this degrades to a SCISSOR of the path's transformed bounds.
        /// One level deep: nested pushes replace the active clip. Balance with
        /// PopClipPath before Clear()/frame end.
        void PushClipPath(const Path& path)
        {
            Array<FlattenedSubPath> subPaths;
            PathFlattener::Flatten(path, GetScaledTolerance(), subPaths);
            if (!m_stencilFills)
            {
                // Fallback: scissor to the transformed bounds - coarse but contained.
                Float2 mn{3.4e38f, 3.4e38f};
                Float2 mx{-3.4e38f, -3.4e38f};
                for (const FlattenedSubPath& sp : subPaths)
                {
                    for (const Float2& pt : sp.points)
                    {
                        const Float2 d = TransformPoint(pt);
                        mn.x = Min(mn.x, d.x);
                        mn.y = Min(mn.y, d.y);
                        mx.x = Max(mx.x, d.x);
                        mx.y = Max(mx.y, d.y);
                    }
                }
                if (mx.x > mn.x && mx.y > mn.y)
                {
                    FlushCurrentCommand();
                    const Rectangle deviceRect{mn.x, mn.y, mx.x - mn.x, mx.y - mn.y};
                    if (m_currentState.clipRect.width > 0.0f &&
                        m_currentState.clipRect.height > 0.0f)
                        m_currentState.clipRect =
                            Rectangle::Intersect(m_currentState.clipRect, deviceRect);
                    else
                        m_currentState.clipRect = deviceRect;
                    m_currentState.clipMode = VGClipMode::Scissor;
                }
                return;
            }

            // The clip geometry itself must not be clipped/scissored by prior state.
            FlushCurrentCommand();
            const VGClipMode savedMode = m_currentState.clipMode;
            m_currentState.clipMode = VGClipMode::None;

            // Winding fans (device-transformed), then the ClipApply quad converts the
            // winding to the 0x80 mask and zeroes the winding bits in one op.
            const Rectangle deviceBounds = EmitWindingFans(subPaths);
            m_clipPathBounds = deviceBounds;
            if (deviceBounds.width <= 0.0f || deviceBounds.height <= 0.0f)
            {
                m_currentState.clipMode = savedMode;
                return;
            }
            EmitDeviceQuad(deviceBounds, VGFillPhase::ClipApply);
            m_clipPathActive = true;
            m_currentState.clipMode = VGClipMode::Stencil;
        }

        /// End the active path clip (zeroes the mask over its bounds).
        void PopClipPath()
        {
            if (!m_clipPathActive)
            {
                m_currentState.clipMode = VGClipMode::None;
                return;
            }
            FlushCurrentCommand();
            m_currentState.clipMode = VGClipMode::None;
            EmitDeviceQuad(m_clipPathBounds, VGFillPhase::ClipClear);
            m_clipPathActive = false;
        }

        /// Pixel snapping: axis-aligned filled rects, borders and horizontal/vertical lines are
        /// snapped to the device pixel grid and drawn WITHOUT the AA fringe (an axis-aligned edge
        /// on a pixel boundary is already crisp and needs no AA). Rotated / curved / diagonal
        /// geometry is untouched. On by default; disable to compare.
        void SetPixelSnapEnabled(bool enabled) { m_pixelSnap = enabled; }
        [[nodiscard]] bool PixelSnapEnabled() const { return m_pixelSnap; }

        // === State Management ===

        void PushState() { m_stateStack.PushBack(m_currentState); }
        void PopState()
        {
            if (!m_stateStack.IsEmpty())
            {
                m_currentState = m_stateStack.Back();
                m_stateStack.PopBack();
            }
        }

        // === Transform ===

        void SetTransform(const Float4x4& transform) { m_currentState.transform = transform; }
        [[nodiscard]] Float4x4 GetTransform() const { return m_currentState.transform; }

        void Translate(f32 x, f32 y)
        {
            m_currentState.transform =
                Float4x4::Translation(Float3{x, y, 0.0f}) * m_currentState.transform;
        }
        void Rotate(f32 radians)
        {
            m_currentState.transform = Float4x4::RotationZ(radians) * m_currentState.transform;
        }
        void Scale(f32 sx, f32 sy)
        {
            m_currentState.transform =
                Float4x4::Scale(Float3{sx, sy, 1.0f}) * m_currentState.transform;
        }
        void ResetTransform() { m_currentState.transform = Float4x4::Identity(); }

        // === Clipping ===

        void PushClipRect(Rectangle rect)
        {
            FlushCurrentCommand();
            m_clipStack.PushBack(m_currentState.clipRect);

            const Rectangle transformedRect = TransformRect(rect);
            if (m_currentState.clipRect.width > 0.0f && m_currentState.clipRect.height > 0.0f)
                m_currentState.clipRect =
                    Rectangle::Intersect(m_currentState.clipRect, transformedRect);
            else
                m_currentState.clipRect = transformedRect;
            m_currentState.clipMode = VGClipMode::Scissor;
        }

        void PopClip()
        {
            FlushCurrentCommand();
            if (!m_clipStack.IsEmpty())
            {
                m_currentState.clipRect = m_clipStack.Back();
                m_clipStack.PopBack();
                m_currentState.clipMode =
                    (m_currentState.clipRect.width > 0.0f && m_currentState.clipRect.height > 0.0f)
                        ? VGClipMode::Scissor
                        : VGClipMode::None;
            }
            else
            {
                m_currentState.clipRect = Rectangle{};
                m_currentState.clipMode = VGClipMode::None;
            }
        }

        // === Opacity ===

        void PushOpacity(f32 opacity)
        {
            m_opacityStack.PushBack(m_currentState.opacity);
            m_currentState.opacity *= Clamp(opacity, 0.0f, 1.0f);
        }
        void PopOpacity()
        {
            if (!m_opacityStack.IsEmpty())
            {
                m_currentState.opacity = m_opacityStack.Back();
                m_opacityStack.PopBack();
            }
            else
            {
                m_currentState.opacity = 1.0f;
            }
        }
        [[nodiscard]] f32 Opacity() const { return m_currentState.opacity; }

        // === Blend Mode ===

        void SetBlendMode(VGBlendMode mode)
        {
            if (mode != m_currentBlendMode)
            {
                FlushCurrentCommand();
                m_currentBlendMode = mode;
            }
        }

        // Switch straight sampling vs MSDF decode; flushes the in-progress command so the mode
        // change lands on a fresh command boundary.
        void SetDrawMode(VGDrawMode mode)
        {
            if (mode != m_currentDrawMode)
            {
                FlushCurrentCommand();
                m_currentDrawMode = mode;
            }
        }

        /// The spread the NEXT gradient draw samples its LUT with (a sampler-address
        /// property of the command, so a change cuts the batch - two fills sharing one
        /// cached ramp may still spread differently).
        void SetGradientSpread(VGGradientSpread spread)
        {
            if (spread != m_currentGradientSpread)
            {
                FlushCurrentCommand();
                m_currentGradientSpread = spread;
            }
        }

        // === Path Drawing ===

        /// Fill a path with a solid color.
        void FillPath(const Path& path, Color color, FillRule fillRule = FillRule::EvenOdd,
                      bool antiAlias = true)
        {
            if (m_stencilFills)
            {
                Array<FlattenedSubPath> subPaths;
                PathFlattener::Flatten(path, GetScaledTolerance(), subPaths);
                if (NeedsStencilFill(subPaths, fillRule))
                {
                    EmitStencilFill(subPaths, fillRule, ApplyOpacity(color), nullptr);
                    return;
                }
            }
            SetupForSolidDraw();
            const usize startVertex = m_batch.vertices.Size();
            const f32 scaledTolerance = GetScaledTolerance();
            FillTessellator::Tessellate(path, fillRule, ApplyOpacity(color), antiAlias,
                                        m_batch.vertices, m_batch.indices, scaledTolerance,
                                        GetScaledFringe());
            TransformVertices(startVertex);
        }

        /// Fill a path with a fill style.
        void FillPath(const Path& path, const IVGFill& fill, FillRule fillRule = FillRule::EvenOdd,
                      bool antiAlias = true)
        {
            if (m_stencilFills)
            {
                Array<FlattenedSubPath> subPaths;
                PathFlattener::Flatten(path, GetScaledTolerance(), subPaths);
                if (NeedsStencilFill(subPaths, fillRule))
                {
                    EmitStencilFill(subPaths, fillRule, Color::White, &fill);
                    return;
                }
            }
            // Gradients bake a ramp LUT bound as the active texture; the tessellator then emits the
            // gradient parameter as a per-vertex texcoord so the ramp is sampled per pixel (exact
            // multi-stop, no 8-bit Gouraud banding). Solid fills stay on the white passthrough.
            const VGGradientTess gradientTess = BindGradientLut(fill);
            const usize startVertex = m_batch.vertices.Size();
            const f32 scaledTolerance = GetScaledTolerance();
            FillTessellator::TessellateWithFill(path, fillRule, fill, antiAlias, m_batch.vertices,
                                                m_batch.indices, scaledTolerance, GetScaledFringe(),
                                                gradientTess);
            ApplyOpacityToVertices(startVertex);
            TransformVertices(startVertex);
            if (gradientTess != VGGradientTess::Gouraud)
            {
                // Restore the default sampling mode so later solid draws aren't stuck on the
                // gradient pipeline / LUT texture.
                SetDrawMode(VGDrawMode::Default);
                SetupForSolidDraw();
            }
        }

        /// Stroke a path with a solid color.
        void StrokePath(const Path& path, Color color, StrokeStyle style,
                        Span<const f32> dashPattern = {}, bool antiAlias = true)
        {
            SetupForSolidDraw();
            const f32 scaledTolerance = GetScaledTolerance();
            Array<FlattenedSubPath> subPaths;
            PathFlattener::Flatten(path, scaledTolerance, subPaths);

            const usize startVertex = m_batch.vertices.Size();
            const Color opColor = ApplyOpacity(color);
            const f32 scaledFringe = GetScaledFringe();

            for (usize s = 0; s < subPaths.Size(); ++s)
            {
                const FlattenedSubPath& subPath = subPaths[s];
                if (subPath.points.Size() < 2)
                    continue;
                StrokeTessellator::Tessellate(
                    Span<const Float2>(subPath.points.Data(), subPath.points.Size()),
                    subPath.isClosed, style, dashPattern, antiAlias, opColor, m_batch.vertices,
                    m_batch.indices, scaledFringe);
            }

            TransformVertices(startVertex);
        }

        // === Convenience: Filled Shapes ===

        void FillRect(Rectangle rect, Color color)
        {
            // Crisp path: snap the axis-aligned rect to the device pixel grid, fill without a
            // fringe (edges on pixel boundaries need no AA).
            if (m_pixelSnap && TransformIsAxisAligned())
            {
                const Float2 p0 = TransformPoint(Float2{rect.x, rect.y});
                const Float2 p1 = TransformPoint(Float2{rect.x + rect.width, rect.y + rect.height});
                SetupForSolidDraw();
                EmitDeviceRect(Round(Min(p0.x, p1.x)), Round(Min(p0.y, p1.y)),
                               Round(Max(p0.x, p1.x)), Round(Max(p0.y, p1.y)), ApplyOpacity(color));
                return;
            }
            PathBuilder pb;
            pb.MoveTo(rect.x, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y + rect.height);
            pb.LineTo(rect.x, rect.y + rect.height);
            pb.Close();
            FillPath(pb.ToPath(), color);
        }

        void FillRoundedRect(Rectangle rect, f32 radius, Color color)
        {
            FillRoundedRect(rect, CornerRadii(radius), color);
        }

        void FillRoundedRect(Rectangle rect, CornerRadii radii, Color color)
        {
            if (radii.IsZero())
            {
                FillRect(rect, color);
                return;
            }
            PathBuilder pb;
            ShapeBuilder::BuildRoundedRect(rect, radii, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillCircle(Float2 center, f32 radius, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildCircle(center, radius, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillEllipse(Float2 center, f32 rx, f32 ry, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildEllipse(center, rx, ry, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillRegularPolygon(Float2 center, f32 radius, i32 sides, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildRegularPolygon(center, radius, sides, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillStar(Float2 center, f32 outerRadius, f32 innerRadius, i32 points, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildStar(center, outerRadius, innerRadius, points, pb);
            FillPath(pb.ToPath(), color);
        }

        // === Convenience: Stroked Shapes ===

        void StrokeRect(Rectangle rect, Color color, f32 width = 1.0f)
        {
            // Crisp path: a centered axis-aligned border drawn as four pixel-snapped bars (no
            // fringe). Top/bottom span the full outer width (owning the corners); the side bars
            // fill vertically BETWEEN them, so coverage is exact with no seams or double-blend.
            if (m_pixelSnap && TransformIsAxisAligned())
            {
                const Float2 p0 = TransformPoint(Float2{rect.x, rect.y});
                const Float2 p1 = TransformPoint(Float2{rect.x + rect.width, rect.y + rect.height});
                const f32 x0 = Min(p0.x, p1.x), x1 = Max(p0.x, p1.x);
                const f32 y0 = Min(p0.y, p1.y), y1 = Max(p0.y, p1.y);
                const Float2 s = DeviceScale();
                const f32 tx = width * s.x, ty = width * s.y;
                const f32 tw = Max(1.0f, Round(tx)), th = Max(1.0f, Round(ty));
                const f32 lx = Round(x0 - tx * 0.5f), rx = Round(x1 - tx * 0.5f);
                const f32 topY = Round(y0 - ty * 0.5f), botY = Round(y1 - ty * 0.5f);
                const Color c = ApplyOpacity(color);
                SetupForSolidDraw();
                EmitDeviceRect(lx, topY, rx + tw, topY + th, c); // top
                EmitDeviceRect(lx, botY, rx + tw, botY + th, c); // bottom
                EmitDeviceRect(lx, topY + th, lx + tw, botY, c); // left
                EmitDeviceRect(rx, topY + th, rx + tw, botY, c); // right
                return;
            }
            PathBuilder pb;
            pb.MoveTo(rect.x, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y + rect.height);
            pb.LineTo(rect.x, rect.y + rect.height);
            pb.Close();
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        void StrokeRoundedRect(Rectangle rect, f32 radius, Color color, f32 width = 1.0f)
        {
            StrokeRoundedRect(rect, CornerRadii(radius), color, width);
        }

        void StrokeRoundedRect(Rectangle rect, CornerRadii radii, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            ShapeBuilder::BuildRoundedRect(rect, radii, pb);
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        void StrokeCircle(Float2 center, f32 radius, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            ShapeBuilder::BuildCircle(center, radius, pb);
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        void StrokeEllipse(Float2 center, f32 rx, f32 ry, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            ShapeBuilder::BuildEllipse(center, rx, ry, pb);
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        // === UI Convenience ===

        void DrawLine(Float2 a, Float2 b, Color color, f32 thickness = 1.0f)
        {
            // Crisp path: a horizontal or vertical line (after the transform) becomes a pixel-
            // snapped bar (no fringe). Diagonals keep the analytical-AA stroke.
            if (m_pixelSnap && TransformIsAxisAligned())
            {
                const Float2 da = TransformPoint(a);
                const Float2 db = TransformPoint(b);
                const Float2 s = DeviceScale();
                if (Abs(da.y - db.y) < 1e-3f) // horizontal
                {
                    const f32 th = Max(1.0f, Round(thickness * s.y));
                    const f32 y0 = Round(da.y - thickness * s.y * 0.5f);
                    SetupForSolidDraw();
                    EmitDeviceRect(Round(Min(da.x, db.x)), y0, Round(Max(da.x, db.x)), y0 + th,
                                   ApplyOpacity(color));
                    return;
                }
                if (Abs(da.x - db.x) < 1e-3f) // vertical
                {
                    const f32 tw = Max(1.0f, Round(thickness * s.x));
                    const f32 x0 = Round(da.x - thickness * s.x * 0.5f);
                    SetupForSolidDraw();
                    EmitDeviceRect(x0, Round(Min(da.y, db.y)), x0 + tw, Round(Max(da.y, db.y)),
                                   ApplyOpacity(color));
                    return;
                }
            }
            PathBuilder pb;
            pb.MoveTo(a.x, a.y);
            pb.LineTo(b.x, b.y);
            StrokePath(pb.ToPath(), color, StrokeStyle(thickness));
        }

        /// Draw a border rectangle with the stroke fully *inside* the rect bounds.
        void DrawBorderRect(Rectangle rect, Color color, f32 thickness = 1.0f)
        {
            const f32 halfThick = thickness * 0.5f;
            const Rectangle insetRect{rect.x + halfThick, rect.y + halfThick,
                                      rect.width - thickness, rect.height - thickness};
            StrokeRect(insetRect, color, thickness);
        }

        void DrawBorderRoundedRect(Rectangle rect, f32 radius, Color color, f32 thickness = 1.0f)
        {
            DrawBorderRoundedRect(rect, CornerRadii(radius), color, thickness);
        }

        void DrawBorderRoundedRect(Rectangle rect, CornerRadii radii, Color color,
                                   f32 thickness = 1.0f)
        {
            const f32 halfThick = thickness * 0.5f;
            const Rectangle insetRect{rect.x + halfThick, rect.y + halfThick,
                                      rect.width - thickness, rect.height - thickness};
            const CornerRadii insetRadii(
                Max(0.0f, radii.topLeft - halfThick), Max(0.0f, radii.topRight - halfThick),
                Max(0.0f, radii.bottomRight - halfThick), Max(0.0f, radii.bottomLeft - halfThick));
            StrokeRoundedRect(insetRect, insetRadii, color, thickness);
        }

        // === Immediate-Mode Path API ===

        void BeginPath() { m_currentPath.Clear(); }

        void MoveTo(f32 x, f32 y) { m_currentPath.MoveTo(x, y); }
        void MoveTo(Float2 point) { m_currentPath.MoveTo(point); }
        void LineTo(f32 x, f32 y) { m_currentPath.LineTo(x, y); }
        void LineTo(Float2 point) { m_currentPath.LineTo(point); }
        void QuadTo(f32 cx, f32 cy, f32 x, f32 y) { m_currentPath.QuadTo(cx, cy, x, y); }
        void QuadTo(Float2 control, Float2 end) { m_currentPath.QuadTo(control, end); }
        void CubicTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y)
        {
            m_currentPath.CubicTo(c1x, c1y, c2x, c2y, x, y);
        }
        void CubicTo(Float2 c1, Float2 c2, Float2 end) { m_currentPath.CubicTo(c1, c2, end); }
        void ArcTo(f32 rx, f32 ry, f32 xAxisRotation, bool largeArc, bool sweep, f32 x, f32 y)
        {
            m_currentPath.ArcTo(rx, ry, xAxisRotation, largeArc, sweep, x, y);
        }
        void ArcTo(f32 rx, f32 ry, f32 xAxisRotation, bool largeArc, bool sweep, Float2 to)
        {
            m_currentPath.ArcTo(rx, ry, xAxisRotation, largeArc, sweep, to);
        }
        void ClosePath() { m_currentPath.Close(); }

        [[nodiscard]] Float2 CurrentPoint() const { return m_currentPath.CurrentPoint(); }

        void Fill(Color color, FillRule fillRule = FillRule::EvenOdd, bool antiAlias = true)
        {
            if (m_currentPath.CommandCount() == 0)
                return;
            FillPath(m_currentPath.ToPath(), color, fillRule, antiAlias);
        }

        void Fill(const IVGFill& fill, FillRule fillRule = FillRule::EvenOdd, bool antiAlias = true)
        {
            if (m_currentPath.CommandCount() == 0)
                return;
            FillPath(m_currentPath.ToPath(), fill, fillRule, antiAlias);
        }

        void Stroke(Color color, StrokeStyle style, Span<const f32> dashPattern = {},
                    bool antiAlias = true)
        {
            if (m_currentPath.CommandCount() == 0)
                return;
            StrokePath(m_currentPath.ToPath(), color, style, dashPattern, antiAlias);
        }

        void Stroke(Color color, f32 thickness = 1.0f)
        {
            if (m_currentPath.CommandCount() == 0)
                return;
            StrokePath(m_currentPath.ToPath(), color, StrokeStyle(thickness));
        }

        // === Images ===

        void DrawImage(const image::ImageData* texture, Float2 position)
        {
            if (texture == nullptr)
                return;
            DrawImage(texture,
                      Rectangle{position.x, position.y, static_cast<f32>(texture->Width()),
                                static_cast<f32>(texture->Height())},
                      Rectangle{0.0f, 0.0f, static_cast<f32>(texture->Width()),
                                static_cast<f32>(texture->Height())},
                      Color::White);
        }

        void DrawImage(const image::ImageData* texture, Float2 position, Color tint)
        {
            if (texture == nullptr)
                return;
            DrawImage(texture,
                      Rectangle{position.x, position.y, static_cast<f32>(texture->Width()),
                                static_cast<f32>(texture->Height())},
                      Rectangle{0.0f, 0.0f, static_cast<f32>(texture->Width()),
                                static_cast<f32>(texture->Height())},
                      tint);
        }

        void DrawImage(const image::ImageData* texture, Rectangle destRect)
        {
            if (texture == nullptr)
                return;
            DrawImage(texture, destRect,
                      Rectangle{0.0f, 0.0f, static_cast<f32>(texture->Width()),
                                static_cast<f32>(texture->Height())},
                      Color::White);
        }

        void DrawImage(const image::ImageData* texture, Rectangle destRect, Rectangle srcRect,
                       Color tint)
        {
            if (texture == nullptr)
                return;

            const i32 textureIndex = GetOrAddTexture(texture);
            SetupForTextureDraw(textureIndex);

            const usize startVertex = m_batch.vertices.Size();
            EmitTexturedQuad(destRect, srcRect, texture->Width(), texture->Height(),
                             ApplyOpacity(tint));
            TransformVertices(startVertex);
        }

        /// DrawImage with the DEST rect snapped to the device pixel grid (axis-aligned
        /// transforms only; rotated draws fall through unsnapped). The crispness half of
        /// the icon-bake pipeline: a baked bitmap drawn at a fractional origin smears
        /// under bilinear sampling and reads differently per instance (the tab-close-X
        /// artifact); snapping makes every instance sample identical texels. The snapped
        /// rect keeps the SOURCE size when it already matches (a baked icon at its bake
        /// size maps 1:1), else rounds the scaled extent.
        void DrawImageSnapped(const image::ImageData* texture, Rectangle destRect,
                              Rectangle srcRect, Color tint = Color::White)
        {
            if (texture == nullptr)
                return;
            if (!(m_pixelSnap && TransformIsAxisAligned()))
            {
                DrawImage(texture, destRect, srcRect, tint);
                return;
            }
            const Float2 p0 = TransformPoint(Float2{destRect.x, destRect.y});
            const Float2 p1 =
                TransformPoint(Float2{destRect.x + destRect.width, destRect.y + destRect.height});
            const f32 x0 = Round(Min(p0.x, p1.x));
            const f32 y0 = Round(Min(p0.y, p1.y));
            const f32 x1 = Round(Max(p0.x, p1.x));
            const f32 y1 = Round(Max(p0.y, p1.y));

            const i32 textureIndex = GetOrAddTexture(texture);
            SetupForTextureDraw(textureIndex);
            const usize startVertex = m_batch.vertices.Size();
            // Device-space quad: emit WITHOUT the transform (positions are final).
            EmitTexturedQuad(Rectangle{x0, y0, x1 - x0, y1 - y0}, srcRect, texture->Width(),
                             texture->Height(), ApplyOpacity(tint));
            (void)startVertex; // no TransformVertices - the points are device-space already
        }

        /// Draw a 9-slice image scaled to fit a destination rectangle.
        void DrawNineSlice(const image::ImageData* texture, Rectangle destRect, Rectangle srcRect,
                           image::NineSlice slices, Color tint)
        {
            if (texture == nullptr)
                return;

            const i32 textureIndex = GetOrAddTexture(texture);
            SetupForTextureDraw(textureIndex);

            const usize startVertex = m_batch.vertices.Size();
            const Color opTint = ApplyOpacity(tint);

            const f32 srcX0 = srcRect.x;
            const f32 srcX1 = srcRect.x + slices.left;
            const f32 srcX2 = srcRect.x + srcRect.width - slices.right;
            const f32 srcY0 = srcRect.y;
            const f32 srcY1 = srcRect.y + slices.top;
            const f32 srcY2 = srcRect.y + srcRect.height - slices.bottom;

            const f32 dstX0 = destRect.x;
            const f32 dstX1 = destRect.x + slices.left;
            const f32 dstX2 = destRect.x + destRect.width - slices.right;
            const f32 dstY0 = destRect.y;
            const f32 dstY1 = destRect.y + slices.top;
            const f32 dstY2 = destRect.y + destRect.height - slices.bottom;

            const u32 tw = texture->Width();
            const u32 th = texture->Height();

            // Row 0 (top).
            EmitTexturedQuad(Rectangle{dstX0, dstY0, slices.left, slices.top},
                             Rectangle{srcX0, srcY0, slices.left, slices.top}, tw, th, opTint);
            EmitTexturedQuad(Rectangle{dstX1, dstY0, dstX2 - dstX1, slices.top},
                             Rectangle{srcX1, srcY0, srcX2 - srcX1, slices.top}, tw, th, opTint);
            EmitTexturedQuad(Rectangle{dstX2, dstY0, slices.right, slices.top},
                             Rectangle{srcX2, srcY0, slices.right, slices.top}, tw, th, opTint);
            // Row 1 (middle).
            EmitTexturedQuad(Rectangle{dstX0, dstY1, slices.left, dstY2 - dstY1},
                             Rectangle{srcX0, srcY1, slices.left, srcY2 - srcY1}, tw, th, opTint);
            EmitTexturedQuad(Rectangle{dstX1, dstY1, dstX2 - dstX1, dstY2 - dstY1},
                             Rectangle{srcX1, srcY1, srcX2 - srcX1, srcY2 - srcY1}, tw, th, opTint);
            EmitTexturedQuad(Rectangle{dstX2, dstY1, slices.right, dstY2 - dstY1},
                             Rectangle{srcX2, srcY1, slices.right, srcY2 - srcY1}, tw, th, opTint);
            // Row 2 (bottom).
            EmitTexturedQuad(Rectangle{dstX0, dstY2, slices.left, slices.bottom},
                             Rectangle{srcX0, srcY2, slices.left, slices.bottom}, tw, th, opTint);
            EmitTexturedQuad(Rectangle{dstX1, dstY2, dstX2 - dstX1, slices.bottom},
                             Rectangle{srcX1, srcY2, srcX2 - srcX1, slices.bottom}, tw, th, opTint);
            EmitTexturedQuad(Rectangle{dstX2, dstY2, slices.right, slices.bottom},
                             Rectangle{srcX2, srcY2, slices.right, slices.bottom}, tw, th, opTint);

            TransformVertices(startVertex);
        }

        // === Text ===

        /// Draw text at a baseline position using a pre-rendered font atlas (low-level).
        void DrawText(StringView text, const fonts::IFontAtlas* atlas,
                      const image::ImageData* atlasTexture, Float2 position, Color color)
        {
            if (text.IsEmpty() || atlas == nullptr || atlasTexture == nullptr)
                return;

            const i32 textureIndex = GetOrAddTexture(atlasTexture);

            // Distance-field atlases decode through the MSDF pipeline: switch the batch's draw
            // mode (flushing the current command) and publish the atlas's DF parameters so the
            // renderer's DF fragment shader can do screen-space anti-aliasing.
            const bool isDF = atlas->Mode() == fonts::AtlasMode::DistanceField;
            if (isDF)
            {
                SetDrawMode(VGDrawMode::DistanceField);
                m_batch.dfPxRange = atlas->DistanceFieldRange();
                m_batch.dfAtlasW = static_cast<f32>(atlas->Width());
                m_batch.dfAtlasH = static_cast<f32>(atlas->Height());
            }

            SetupForTextureDraw(textureIndex);

            const usize startVertex = m_batch.vertices.Size();
            const Color opColor = ApplyOpacity(color);
            f32 cursorX = position.x;
            const f32 cursorY = position.y;

            usize ci = 0;
            while (ci < text.Size())
            {
                const i32 ch = static_cast<i32>(fonts::DecodeCodepoint(text, ci));
                fonts::GlyphQuad quad;
                if (atlas->GetGlyphQuad(ch, cursorX, cursorY, quad))
                    EmitGlyphQuad(quad, opColor);
            }

            TransformVertices(startVertex);

            if (isDF)
                SetDrawMode(VGDrawMode::Default);
        }

        /// Draw text with horizontal alignment within bounds (vertically centered).
        void DrawText(StringView text, const fonts::IFont* font, const fonts::IFontAtlas* atlas,
                      const image::ImageData* atlasTexture, Rectangle bounds,
                      fonts::TextAlignment align, Color color)
        {
            if (text.IsEmpty() || font == nullptr)
                return;

            const f32 textWidth = font->MeasureString(text);
            f32 offsetX = bounds.x;
            switch (align)
            {
            case fonts::TextAlignment::Left:
                offsetX = bounds.x;
                break;
            case fonts::TextAlignment::Center:
                offsetX = bounds.x + (bounds.width - textWidth) * 0.5f;
                break;
            case fonts::TextAlignment::Right:
                offsetX = bounds.x + bounds.width - textWidth;
                break;
            }

            const fonts::FontMetrics fm = font->Metrics();
            const f32 offsetY = bounds.y + (bounds.height - fm.lineHeight) * 0.5f + fm.ascent;
            DrawText(text, atlas, atlasTexture, Float2{offsetX, offsetY}, color);
        }

        /// Draw text with horizontal and vertical alignment within bounds.
        void DrawText(StringView text, const fonts::IFont* font, const fonts::IFontAtlas* atlas,
                      const image::ImageData* atlasTexture, Rectangle bounds,
                      fonts::TextAlignment hAlign, fonts::VerticalAlignment vAlign, Color color)
        {
            if (text.IsEmpty() || font == nullptr)
                return;

            const f32 textWidth = font->MeasureString(text);
            f32 offsetX = bounds.x;
            f32 offsetY = bounds.y;

            switch (hAlign)
            {
            case fonts::TextAlignment::Left:
                offsetX = bounds.x;
                break;
            case fonts::TextAlignment::Center:
                offsetX = bounds.x + (bounds.width - textWidth) * 0.5f;
                break;
            case fonts::TextAlignment::Right:
                offsetX = bounds.x + bounds.width - textWidth;
                break;
            }

            const fonts::FontMetrics fm = font->Metrics();
            switch (vAlign)
            {
            case fonts::VerticalAlignment::Top:
                offsetY = bounds.y + fm.ascent;
                break;
            case fonts::VerticalAlignment::Middle:
                offsetY = bounds.y + (bounds.height - fm.lineHeight) * 0.5f + fm.ascent;
                break;
            case fonts::VerticalAlignment::Bottom:
                offsetY = bounds.y + bounds.height - fm.descent;
                break;
            case fonts::VerticalAlignment::Baseline:
                offsetY = bounds.y;
                break;
            }

            DrawText(text, atlas, atlasTexture, Float2{offsetX, offsetY}, color);
        }

        /// Convenience: draw text using a CachedFont (requires a FontService).
        void DrawText(StringView text, fonts::CachedFont* font, Float2 position, Color color)
        {
            if (font == nullptr || m_fontService == nullptr)
                return;
            image::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr)
                return;
            DrawText(text, font->atlas, atlasTex, position, color);
        }

        /// Convenience: draw text using a CachedFont with alignment.
        void DrawText(StringView text, fonts::CachedFont* font, Rectangle bounds,
                      fonts::TextAlignment hAlign, fonts::VerticalAlignment vAlign, Color color)
        {
            if (font == nullptr || m_fontService == nullptr)
                return;
            image::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr)
                return;
            DrawText(text, font->font, font->atlas, atlasTex, bounds, hAlign, vAlign, color);
        }

        /// Draw pre-shaped glyphs at an offset (for scroll-offset text rendering).
        void DrawPositionedGlyphs(const Array<fonts::GlyphPosition>& positions,
                                  fonts::CachedFont* font, f32 offsetX, f32 offsetY, Color color)
        {
            if (positions.IsEmpty() || font == nullptr || m_fontService == nullptr)
                return;
            image::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr || font->atlas == nullptr)
                return;

            const i32 textureIndex = GetOrAddTexture(atlasTex);

            // Same DF handling as DrawText: MSDF atlases decode through the distance-field
            // pipeline, or the raw field channels render as rainbow glyphs. This is the
            // SHAPED path every real control uses (EditText, wrapped text), so without this
            // branch the UI could never use MSDF fonts.
            const bool isDF = font->atlas->Mode() == fonts::AtlasMode::DistanceField;
            if (isDF)
            {
                SetDrawMode(VGDrawMode::DistanceField);
                m_batch.dfPxRange = font->atlas->DistanceFieldRange();
                m_batch.dfAtlasW = static_cast<f32>(font->atlas->Width());
                m_batch.dfAtlasH = static_cast<f32>(font->atlas->Height());
            }

            SetupForTextureDraw(textureIndex);

            const usize startVertex = m_batch.vertices.Size();
            const Color opColor = ApplyOpacity(color);

            for (usize p = 0; p < positions.Size(); ++p)
            {
                const fonts::GlyphPosition& pos = positions[p];
                fonts::GlyphQuad quad;
                f32 glyphX = offsetX + pos.x;
                if (font->atlas->GetGlyphQuad(pos.codepoint, glyphX, offsetY + pos.y, quad))
                    EmitGlyphQuad(quad, opColor);
            }

            TransformVertices(startVertex);

            if (isDF)
                SetDrawMode(VGDrawMode::Default);
        }

        /// Draw text with word wrapping. Position is the top-left of the text block.
        void DrawTextWrapped(StringView text, fonts::CachedFont* font, Float2 position,
                             f32 maxWidth, Color color,
                             fonts::TextAlignment hAlign = fonts::TextAlignment::Left)
        {
            if (text.IsEmpty() || font == nullptr || font->shaper == nullptr ||
                m_fontService == nullptr)
                return;
            image::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr)
                return;

            Array<fonts::GlyphPosition> positions;
            f32 totalHeight = 0.0f;
            if (!font->shaper->ShapeTextWrapped(*font->font, text, maxWidth, positions, totalHeight)
                     .IsOk())
                return;

            if (hAlign != fonts::TextAlignment::Left)
                ApplyLineAlignment(positions, maxWidth, hAlign);

            DrawPositionedGlyphs(positions, font, position.x,
                                 position.y + font->font->Metrics().ascent, color);
        }

        void DrawTextWrapped(StringView text, fonts::CachedFont* font, Rectangle bounds,
                             Color color, fonts::TextAlignment hAlign = fonts::TextAlignment::Left)
        {
            DrawTextWrapped(text, font, Float2{bounds.x, bounds.y}, bounds.width, color, hAlign);
        }

        /// Measure wrapped text without drawing. Returns total height (0 if no shaper).
        [[nodiscard]] f32 MeasureTextWrapped(StringView text, fonts::CachedFont* font, f32 maxWidth)
        {
            if (text.IsEmpty() || font == nullptr || font->shaper == nullptr)
                return 0.0f;

            Array<fonts::GlyphPosition> positions;
            f32 totalHeight = 0.0f;
            if (!font->shaper->ShapeTextWrapped(*font->font, text, maxWidth, positions, totalHeight)
                     .IsOk())
                return 0.0f;
            return totalHeight;
        }

        /// Draw text using the default font at the given pixel size (requires a FontService).
        void DrawText(StringView text, f32 fontSize, Float2 position, Color color)
        {
            if (text.IsEmpty() || m_fontService == nullptr)
                return;
            fonts::CachedFont* font = m_fontService->GetFont(fontSize);
            if (font == nullptr)
                return;
            DrawText(text, font, position, color);
        }

        /// Fill a polygon defined by a span of points.
        void FillPolygon(Span<const Float2> points, Color color)
        {
            if (points.Size() < 3)
                return;

            BeginPath();
            MoveTo(points[0]);
            for (usize i = 1; i < points.Size(); ++i)
                LineTo(points[i]);
            ClosePath();
            Fill(color);
        }

        /// Measure the width and line height of a string in pixels.
        [[nodiscard]] Float2 MeasureText(StringView text, const fonts::IFont* font)
        {
            if (font == nullptr)
                return Float2::Zero;
            return Float2{font->MeasureString(text), font->Metrics().lineHeight};
        }

        /// Measure just the pixel width of a string.
        [[nodiscard]] f32 MeasureTextWidth(StringView text, const fonts::IFont* font)
        {
            if (font == nullptr)
                return 0.0f;
            return font->MeasureString(text);
        }

    private:
        /// Applies horizontal alignment offsets to shaped glyph positions per line.
        static void ApplyLineAlignment(Array<fonts::GlyphPosition>& positions, f32 maxWidth,
                                       fonts::TextAlignment align)
        {
            if (positions.IsEmpty())
                return;

            usize lineStart = 0;
            f32 lineY = positions[0].y;

            for (usize i = 0; i <= positions.Size(); ++i)
            {
                const bool newLine = (i == positions.Size()) || (positions[i].y != lineY);
                if (newLine)
                {
                    if (lineStart < i)
                    {
                        const fonts::GlyphPosition& lastGlyph = positions[i - 1];
                        const f32 lineWidth = lastGlyph.x + lastGlyph.advance;
                        f32 offset = 0.0f;
                        if (align == fonts::TextAlignment::Center)
                            offset = (maxWidth - lineWidth) * 0.5f;
                        else if (align == fonts::TextAlignment::Right)
                            offset = maxWidth - lineWidth;

                        if (offset != 0.0f)
                        {
                            for (usize j = lineStart; j < i; ++j)
                                positions[j].x += offset;
                        }
                    }

                    if (i < positions.Size())
                    {
                        lineStart = i;
                        lineY = positions[i].y;
                    }
                }
            }
        }

        /// Emit a glyph quad into the batch in untransformed coordinates.
        void EmitGlyphQuad(const fonts::GlyphQuad& quad, Color color)
        {
            const u32 baseIndex = static_cast<u32>(m_batch.vertices.Size());

            m_batch.vertices.PushBack(
                VGVertex(Float2{quad.x0, quad.y0}, Float2{quad.u0, quad.v0}, color, 1.0f));
            m_batch.vertices.PushBack(
                VGVertex(Float2{quad.x1, quad.y0}, Float2{quad.u1, quad.v0}, color, 1.0f));
            m_batch.vertices.PushBack(
                VGVertex(Float2{quad.x1, quad.y1}, Float2{quad.u1, quad.v1}, color, 1.0f));
            m_batch.vertices.PushBack(
                VGVertex(Float2{quad.x0, quad.y1}, Float2{quad.u0, quad.v1}, color, 1.0f));

            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 1);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 3);
        }

        /// Emit a textured quad into the batch in untransformed coordinates (coverage 1.0).
        void EmitTexturedQuad(Rectangle destRect, Rectangle srcRect, u32 texWidth, u32 texHeight,
                              Color color)
        {
            if (destRect.width <= 0.0f || destRect.height <= 0.0f)
                return;

            const u32 baseIndex = static_cast<u32>(m_batch.vertices.Size());

            const f32 u0 = srcRect.x / static_cast<f32>(texWidth);
            const f32 v0 = srcRect.y / static_cast<f32>(texHeight);
            const f32 u1 = (srcRect.x + srcRect.width) / static_cast<f32>(texWidth);
            const f32 v1 = (srcRect.y + srcRect.height) / static_cast<f32>(texHeight);

            m_batch.vertices.PushBack(
                VGVertex(Float2{destRect.x, destRect.y}, Float2{u0, v0}, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Float2{destRect.x + destRect.width, destRect.y},
                                               Float2{u1, v0}, color, 1.0f));
            m_batch.vertices.PushBack(
                VGVertex(Float2{destRect.x + destRect.width, destRect.y + destRect.height},
                         Float2{u1, v1}, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Float2{destRect.x, destRect.y + destRect.height},
                                               Float2{u0, v1}, color, 1.0f));

            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 1);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 3);
        }

        /// Look up a texture in the batch or append it. Index 0 is the white texture.
        i32 GetOrAddTexture(const image::ImageData* tex)
        {
            if (tex == nullptr)
                return 0;
            for (usize i = 0; i < m_batch.textures.Size(); ++i)
                if (m_batch.textures[i] == tex)
                    return static_cast<i32>(i);
            m_batch.textures.PushBack(tex);
            return static_cast<i32>(m_batch.textures.Size() - 1);
        }

        // Bake a gradient's color ramp into a 256x1 RGBA8/sRGB LUT, bind it as the active texture,
        // and select the draw mode. Returns the tessellation emit mode (Gouraud for solid fills).
        // The LUT lives in the persistent content-keyed cache (see m_gradientLutCache);
        // the GPU decodes the sRGB texels to linear on sample, matching the vertex-color path.
        // Linear gradients stay on the Default pipeline (affine LUT parameter is exact); radial/
        // conic upgrade to their per-pixel pipelines only when the host enabled per-pixel gradients
        // (i.e. wired the shaders) - otherwise they fall back to the same affine LUT approximation.
        [[nodiscard]] VGGradientTess BindGradientLut(const IVGFill& fill)
        {
            if (!fill.RequiresInterpolation())
            {
                SetDrawMode(VGDrawMode::Default);
                SetGradientSpread(VGGradientSpread::Pad);
                SetupForSolidDraw();
                return VGGradientTess::Gouraud;
            }

            constexpr u32 kLutWidth = 256;
            u8 pixels[kLutWidth * 4];
            for (u32 i = 0; i < kLutWidth; ++i)
            {
                const f32 t = static_cast<f32>(i) / static_cast<f32>(kLutWidth - 1);
                const Color32 c = ToColor32(fill.SampleRamp(t));
                pixels[i * 4 + 0] = c.r;
                pixels[i * 4 + 1] = c.g;
                pixels[i * 4 + 2] = c.b;
                pixels[i * 4 + 3] = c.a;
            }

            // Content-keyed persistent cache: identical ramps (N fills of one gradient, and
            // the same gradient across frames) share ONE ImageData - the stable identity the
            // renderer's GPU texture cache keys on. A per-frame pool here caused freed-and-
            // reallocated LUTs to cache-hit stale GPU ramps (or leak one texture per fill).
            const u64 rampHash = HashBytes(pixels, sizeof(pixels));
            image::OwnedImageData* raw = nullptr;
            if (UniquePtr<image::OwnedImageData>* cached = m_gradientLutCache.Find(rampHash))
            {
                raw = cached->Get();
            }
            else
            {
                UniquePtr<image::OwnedImageData> lut = MakeUnique<image::OwnedImageData>(
                    DefaultAllocator(), kLutWidth, 1u, image::PixelFormat::RGBA8,
                    Span<const u8>(pixels, kLutWidth * 4));
                raw = lut.Get();
                m_gradientLutCache.InsertOrAssign(rampHash, Move(lut));
            }

            VGGradientTess tess = VGGradientTess::LinearLut;
            VGDrawMode mode = VGDrawMode::Default;
            if (m_perPixelGradients)
            {
                switch (fill.GradientKind())
                {
                case VGGradientKind::Radial:
                    tess = VGGradientTess::RadialCoord;
                    mode = VGDrawMode::GradientRadial;
                    break;
                case VGGradientKind::Conic:
                    tess = VGGradientTess::ConicCoord;
                    mode = VGDrawMode::GradientConic;
                    break;
                default:
                    break;
                }
            }
            SetDrawMode(mode);
            SetGradientSpread(fill.Spread());
            SetupForTextureDraw(GetOrAddTexture(raw));
            return tess;
        }

        void SetupForSolidDraw()
        {
            if (m_currentTextureIndex != 0)
            {
                FlushCurrentCommand();
                m_currentTextureIndex = 0;
            }
        }

        void SetupForTextureDraw(i32 textureIndex)
        {
            if (m_currentTextureIndex != textureIndex)
            {
                FlushCurrentCommand();
                m_currentTextureIndex = textureIndex;
            }
        }

        // === Stencil-then-cover emission (complex fills; see SetStencilFills) ===

        /// True when a fill's contours cannot be drawn correctly by the direct tessellator:
        /// multiple contours (holes / disjoint pieces) or a non-convex single contour
        /// (self-intersection, concavity - ear clipping mis-fills both under either rule).
        [[nodiscard]] static bool NeedsStencilFill(const Array<FlattenedSubPath>& subPaths,
                                                   FillRule /*fillRule*/)
        {
            usize contours = 0;
            const FlattenedSubPath* single = nullptr;
            for (const FlattenedSubPath& subPath : subPaths)
            {
                if (subPath.points.Size() >= 3)
                {
                    ++contours;
                    single = &subPath;
                }
            }
            if (contours == 0)
            {
                return false;
            }
            if (contours > 1)
            {
                return true;
            }
            return !IsConvexSimpleLoop(single->points);
        }

        /// Convex AND simple: every turn has the same sign and the winding totals exactly
        /// one revolution (a pentagram-style loop turns 2+ revolutions and would slip past
        /// a sign-only test; under NonZero its core must fill, under EvenOdd it must not -
        /// both need the stencil).
        [[nodiscard]] static bool IsConvexSimpleLoop(const Array<Float2>& points)
        {
            usize n = points.Size();
            // Tolerate an explicitly closed polyline (last point repeats the first).
            if (n >= 2 && points[0].x == points[n - 1].x && points[0].y == points[n - 1].y)
            {
                --n;
            }
            if (n < 3)
            {
                return true; // degenerate - nothing the stencil would improve
            }
            f32 turnSign = 0.0f;
            f32 totalTurn = 0.0f;
            for (usize i = 0; i < n; ++i)
            {
                const Float2 a = points[i];
                const Float2 b = points[(i + 1) % n];
                const Float2 c = points[(i + 2) % n];
                const Float2 ab{b.x - a.x, b.y - a.y};
                const Float2 bc{c.x - b.x, c.y - b.y};
                const f32 cross = ab.x * bc.y - ab.y * bc.x;
                const f32 dot = ab.x * bc.x + ab.y * bc.y;
                if (Abs(cross) > 1e-6f)
                {
                    const f32 sign = cross > 0.0f ? 1.0f : -1.0f;
                    if (turnSign == 0.0f)
                    {
                        turnSign = sign;
                    }
                    else if (sign != turnSign)
                    {
                        return false;
                    }
                }
                totalTurn += Atan2(cross, dot);
            }
            return Abs(Abs(totalTurn) - 2.0f * kPi) < 0.1f;
        }

        /// Emit a stencil-then-cover fill: a color-masked winding pass (per-contour fans
        /// from each contour's first point) then a bounding-quad cover carrying the fill's
        /// color/gradient data. `fill` null = solid `solidColor` (already opacity-applied);
        /// non-null follows the same gradient plumbing as the tessellated path.
        /// Emit color-masked winding fans (one per contour, device-transformed) as a
        /// StencilWrite command. Returns the DEVICE-space bounds of the emitted points
        /// (zero-sized when degenerate). Shared by stencil fills and PushClipPath.
        Rectangle EmitWindingFans(const Array<FlattenedSubPath>& subPaths,
                                  FillRule fillRule = FillRule::NonZero)
        {
            SetDrawMode(VGDrawMode::Default);
            SetupForSolidDraw();
            FlushCurrentCommand();
            const usize writeStartVertex = m_batch.vertices.Size();
            const i32 writeStartIndex = static_cast<i32>(m_batch.indices.Size());
            for (const FlattenedSubPath& subPath : subPaths)
            {
                const usize n = subPath.points.Size();
                if (n < 3)
                {
                    continue;
                }
                const u32 base = static_cast<u32>(m_batch.vertices.Size());
                for (const Float2& pt : subPath.points)
                {
                    m_batch.vertices.PushBack(
                        VGVertex(pt, Float2{VGVertex::SolidUV, VGVertex::SolidUV}, Color::White));
                }
                for (usize i = 1; i + 1 < n; ++i)
                {
                    m_batch.indices.PushBack(base);
                    m_batch.indices.PushBack(base + static_cast<u32>(i));
                    m_batch.indices.PushBack(base + static_cast<u32>(i) + 1);
                }
            }
            TransformVertices(writeStartVertex);
            Float2 mn{3.4e38f, 3.4e38f};
            Float2 mx{-3.4e38f, -3.4e38f};
            for (usize i = writeStartVertex; i < m_batch.vertices.Size(); ++i)
            {
                const Float2 pt = m_batch.vertices[i].position;
                mn.x = Min(mn.x, pt.x);
                mn.y = Min(mn.y, pt.y);
                mx.x = Max(mx.x, pt.x);
                mx.y = Max(mx.y, pt.y);
            }
            PushExplicitCommand(writeStartIndex, VGFillPhase::StencilWrite, fillRule);
            if (mx.x <= mn.x || mx.y <= mn.y)
            {
                return Rectangle{0.0f, 0.0f, 0.0f, 0.0f};
            }
            return Rectangle{mn.x, mn.y, mx.x - mn.x, mx.y - mn.y};
        }

        /// Emit a color-masked DEVICE-space quad as `phase` (ClipApply / ClipClear -
        /// no vertex transform: the rect is already in device coordinates).
        void EmitDeviceQuad(Rectangle rect, VGFillPhase phase)
        {
            SetDrawMode(VGDrawMode::Default);
            SetupForSolidDraw();
            FlushCurrentCommand();
            const i32 startIndex = static_cast<i32>(m_batch.indices.Size());
            const u32 base = static_cast<u32>(m_batch.vertices.Size());
            const Float2 corners[4] = {Float2{rect.x, rect.y},
                                       Float2{rect.x + rect.width, rect.y},
                                       Float2{rect.x + rect.width, rect.y + rect.height},
                                       Float2{rect.x, rect.y + rect.height}};
            for (const Float2& corner : corners)
            {
                m_batch.vertices.PushBack(VGVertex(
                    corner, Float2{VGVertex::SolidUV, VGVertex::SolidUV}, Color::White));
            }
            const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
            for (u32 index : quad)
            {
                m_batch.indices.PushBack(index);
            }
            PushExplicitCommand(startIndex, phase, FillRule::NonZero);
        }

        void EmitStencilFill(const Array<FlattenedSubPath>& subPaths, FillRule fillRule,
                             Color solidColor, const IVGFill* fill)
        {
            Float2 boundsMin{3.4e38f, 3.4e38f};
            Float2 boundsMax{-3.4e38f, -3.4e38f};
            usize totalPoints = 0;
            for (const FlattenedSubPath& subPath : subPaths)
            {
                for (const Float2& pt : subPath.points)
                {
                    boundsMin.x = Min(boundsMin.x, pt.x);
                    boundsMin.y = Min(boundsMin.y, pt.y);
                    boundsMax.x = Max(boundsMax.x, pt.x);
                    boundsMax.y = Max(boundsMax.y, pt.y);
                }
                totalPoints += subPath.points.Size();
            }
            if (totalPoints < 3 || boundsMax.x <= boundsMin.x || boundsMax.y <= boundsMin.y)
            {
                return;
            }
            const Rectangle bounds{boundsMin.x, boundsMin.y, boundsMax.x - boundsMin.x,
                                   boundsMax.y - boundsMin.y};

            // --- Winding pass (shared emitter; fans transformed to device space).
            (void)EmitWindingFans(subPaths, fillRule);

            // --- Cover pass: a bounds quad with the fill's shading. Gradient fills bind
            // their LUT / per-pixel mode exactly like the tessellated path.
            VGGradientTess gradientTess = VGGradientTess::Gouraud;
            if (fill != nullptr)
            {
                gradientTess = BindGradientLut(*fill);
            }
            const usize coverStartVertex = m_batch.vertices.Size();
            const i32 coverStartIndex = static_cast<i32>(m_batch.indices.Size());
            const Float2 corners[4] = {Float2{bounds.x, bounds.y},
                                       Float2{bounds.x + bounds.width, bounds.y},
                                       Float2{bounds.x + bounds.width, bounds.y + bounds.height},
                                       Float2{bounds.x, bounds.y + bounds.height}};
            const u32 coverBase = static_cast<u32>(m_batch.vertices.Size());
            for (const Float2& corner : corners)
            {
                Float2 uv{VGVertex::SolidUV, VGVertex::SolidUV};
                Color color = solidColor;
                if (fill != nullptr)
                {
                    if (gradientTess == VGGradientTess::Gouraud)
                    {
                        // No gradient shader available: affine corner colors (the same
                        // approximation the Gouraud tessellated path uses).
                        color = fill->GetColorAt(corner, bounds);
                    }
                    else
                    {
                        uv = FillTessellator::GradientTexCoord(gradientTess, *fill, corner,
                                                               bounds);
                        color = Color::White;
                    }
                }
                m_batch.vertices.PushBack(VGVertex(corner, uv, color));
            }
            const u32 quad[6] = {coverBase,     coverBase + 1, coverBase + 2,
                                 coverBase,     coverBase + 2, coverBase + 3};
            for (u32 index : quad)
            {
                m_batch.indices.PushBack(index);
            }
            if (fill != nullptr)
            {
                ApplyOpacityToVertices(coverStartVertex);
            }
            TransformVertices(coverStartVertex);
            PushExplicitCommand(coverStartIndex, VGFillPhase::StencilCover, fillRule);

            if (gradientTess != VGGradientTess::Gouraud)
            {
                // Restore default sampling so later solid draws aren't stuck on the
                // gradient pipeline / LUT texture (mirrors FillPath's gradient epilogue).
                SetDrawMode(VGDrawMode::Default);
                SetupForSolidDraw();
            }
        }

        /// Push a command for indices emitted since `startIndex` with an explicit fill
        /// phase, using the context's current state for everything else. Used by the
        /// stencil-then-cover emitter, whose two passes can never merge with neighbors.
        void PushExplicitCommand(i32 startIndex, VGFillPhase phase, FillRule fillRule)
        {
            const i32 indexCount = static_cast<i32>(m_batch.indices.Size()) - startIndex;
            if (indexCount <= 0)
            {
                return;
            }
            VGCommand cmd;
            cmd.startIndex = startIndex;
            cmd.indexCount = indexCount;
            cmd.textureIndex = m_currentTextureIndex;
            cmd.clipRect = m_currentState.clipRect;
            cmd.blendMode = m_currentBlendMode;
            cmd.clipMode = m_currentState.clipMode;
            cmd.stencilRef = m_currentState.stencilRef;
            cmd.drawMode = m_currentDrawMode;
            cmd.gradientSpread = m_currentGradientSpread;
            cmd.fillPhase = phase;
            cmd.fillRule = fillRule;
            m_batch.commands.PushBack(cmd);
            m_commandStartIndex = static_cast<i32>(m_batch.indices.Size());
        }

        void FlushCurrentCommand()
        {
            const i32 indexCount = static_cast<i32>(m_batch.indices.Size()) - m_commandStartIndex;
            if (indexCount > 0)
            {
                VGCommand cmd;
                cmd.startIndex = m_commandStartIndex;
                cmd.indexCount = indexCount;
                cmd.textureIndex = m_currentTextureIndex;
                cmd.clipRect = m_currentState.clipRect;
                cmd.blendMode = m_currentBlendMode;
                cmd.clipMode = m_currentState.clipMode;
                cmd.stencilRef = m_currentState.stencilRef;
                cmd.drawMode = m_currentDrawMode;
                cmd.gradientSpread = m_currentGradientSpread;

                m_batch.commands.PushBack(cmd);
                m_commandStartIndex = static_cast<i32>(m_batch.indices.Size());
            }
        }

        /// Tolerance adjusted for current transform scale (tighter when scaled up).
        [[nodiscard]] f32 GetScaledTolerance() const
        {
            if (m_currentState.transform == Float4x4::Identity())
                return m_tolerance;

            const f32 sx =
                Length(Float2{m_currentState.transform(0, 0), m_currentState.transform(0, 1)});
            const f32 sy =
                Length(Float2{m_currentState.transform(1, 0), m_currentState.transform(1, 1)});
            const f32 scale = Max(sx, sy);

            if (scale > 0.0001f)
                return m_tolerance / scale;
            return m_tolerance;
        }

        // The AA fringe width to hand the tessellators, kept ~constant in SCREEN pixels. The
        // fringe geometry is built in content space and then scaled by the current transform, so
        // pre-divide by that scale (mirrors GetScaledTolerance) - otherwise a scaled-up shape gets
        // a proportionally wider, blurrier edge.
        [[nodiscard]] f32 GetScaledFringe() const
        {
            constexpr f32 kBaseFringe = 0.75f;
            if (m_currentState.transform == Float4x4::Identity())
                return kBaseFringe;

            const f32 sx =
                Length(Float2{m_currentState.transform(0, 0), m_currentState.transform(0, 1)});
            const f32 sy =
                Length(Float2{m_currentState.transform(1, 0), m_currentState.transform(1, 1)});
            const f32 scale = Max(sx, sy);
            return (scale > 0.0001f) ? kBaseFringe / scale : kBaseFringe;
        }

        // === Pixel snapping (crisp axis-aligned rects/lines) ===

        // The current transform has no rotation/skew (its linear part is diagonal), so device
        // axis-aligned geometry maps to axis-aligned pixels - safe to snap to the pixel grid.
        [[nodiscard]] bool TransformIsAxisAligned() const
        {
            const Float4x4& m = m_currentState.transform;
            return Abs(m(0, 1)) < 1e-4f && Abs(m(1, 0)) < 1e-4f;
        }
        // Per-axis device scale (magnitude of the transform's x/y basis; valid when axis-aligned).
        [[nodiscard]] Float2 DeviceScale() const
        {
            const Float4x4& m = m_currentState.transform;
            return Float2{Abs(m(0, 0)), Abs(m(1, 1))};
        }
        // Emit a crisp (NO fringe) filled rect whose corners are already in DEVICE space; the
        // caller has snapped the edges to the pixel grid, so no anti-aliasing is needed.
        void EmitDeviceRect(f32 x0, f32 y0, f32 x1, f32 y1, Color color)
        {
            if (x1 <= x0 || y1 <= y0)
                return;
            const u32 base = static_cast<u32>(m_batch.vertices.Size());
            m_batch.vertices.PushBack(VGVertex::Solid(Float2{x0, y0}, color));
            m_batch.vertices.PushBack(VGVertex::Solid(Float2{x1, y0}, color));
            m_batch.vertices.PushBack(VGVertex::Solid(Float2{x1, y1}, color));
            m_batch.vertices.PushBack(VGVertex::Solid(Float2{x0, y1}, color));
            m_batch.indices.PushBack(base + 0);
            m_batch.indices.PushBack(base + 1);
            m_batch.indices.PushBack(base + 2);
            m_batch.indices.PushBack(base + 0);
            m_batch.indices.PushBack(base + 2);
            m_batch.indices.PushBack(base + 3);
        }

        [[nodiscard]] Float2 TransformPoint(Float2 point) const
        {
            if (m_currentState.transform == Float4x4::Identity())
                return point;
            return TransformPoint2D(point, m_currentState.transform);
        }

        void TransformVertices(usize startVertex)
        {
            if (m_currentState.transform == Float4x4::Identity())
                return;

            for (usize i = startVertex; i < m_batch.vertices.Size(); ++i)
                m_batch.vertices[i].position =
                    TransformPoint2D(m_batch.vertices[i].position, m_currentState.transform);
        }

        [[nodiscard]] Color ApplyOpacity(Color color) const
        {
            if (m_currentState.opacity >= 1.0f)
                return color;
            return Color{color.r, color.g, color.b, color.a * m_currentState.opacity};
        }

        void ApplyOpacityToVertices(usize startVertex)
        {
            if (m_currentState.opacity >= 1.0f)
                return;
            // Vertices store full float Color; apply opacity in place.
            for (usize i = startVertex; i < m_batch.vertices.Size(); ++i)
                m_batch.vertices[i].color = ApplyOpacity(m_batch.vertices[i].color);
        }

        [[nodiscard]] Rectangle TransformRect(Rectangle rect) const
        {
            if (m_currentState.transform == Float4x4::Identity())
                return rect;

            const Float2 topLeft = TransformPoint(Float2{rect.x, rect.y});
            const Float2 bottomRight =
                TransformPoint(Float2{rect.x + rect.width, rect.y + rect.height});
            return Rectangle{topLeft.x, topLeft.y, bottomRight.x - topLeft.x,
                             bottomRight.y - topLeft.y};
        }

        VGBatch m_batch;
        image::OwnedImageData m_whiteTexture;
        // Per-frame baked gradient ramp LUTs (owned; UniquePtr for stable addresses since the
        // batch textures hold raw pointers). Cleared on Clear() once the batch is consumed.
        // Gradient ramp LUTs, keyed by ramp CONTENT hash, persisted across frames (stable
        // ImageData identities for the renderer's texture cache). Over-budget eviction is
        // whole-cache, announced through VGBatch::evictedTextures, with the images held one
        // frame (m_evictedLutHold) so the announced keys are not dangling in flight.
        HashMap<u64, UniquePtr<image::OwnedImageData>> m_gradientLutCache;
        Array<UniquePtr<image::OwnedImageData>> m_evictedLutHold;
        PathBuilder m_currentPath;
        fonts::IFontService* m_fontService = nullptr;

        Array<VGState> m_stateStack;
        Rectangle m_clipPathBounds{}; // DEVICE-space bounds of the active stencil clip
        bool m_clipPathActive = false;
        VGState m_currentState;

        Array<Rectangle> m_clipStack;
        Array<f32> m_opacityStack;

        VGBlendMode m_currentBlendMode = VGBlendMode::Normal;
        VGDrawMode m_currentDrawMode = VGDrawMode::Default;
        VGGradientSpread m_currentGradientSpread = VGGradientSpread::Pad;
        bool m_perPixelGradients = false;
        bool m_stencilFills = false; // host opt-in: stencil-then-cover for complex fills // radial/conic use dedicated per-pixel shaders when set
        i32 m_currentTextureIndex = 0;
        i32 m_commandStartIndex = 0;
        f32 m_tolerance = 0.05f;
        bool m_pixelSnap = true;
    };
}
