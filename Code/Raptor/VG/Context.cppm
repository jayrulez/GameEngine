// Raptor::VG — :context partition.
//
// VGState (the per-state-stack snapshot) and VGContext — the main user-facing
// vector-graphics API. Immediate-mode drawing of paths, shapes, images, 9-slice,
// and text; produces a batched VGBatch for an external renderer to consume.
// Ported from Sedulous.VG (VGState/VGContext). The 2D transform is a Mat4
// (mirroring Sedulous's 4x4 Matrix); colors are float Color (packed to Color32 only at the vertex).

module;
#include "Core/Prelude.h"

export module raptor.vg:context;

import raptor.core;
import raptor.image;
import raptor.fonts;
import :enums;
import :vertex;
import :style;
import :fills;
import :path;
import :shapes;
import :tessellation;
import :batch;

using namespace raptor::core;

export namespace raptor::vg
{
    namespace img = raptor::image;
    namespace fonts = raptor::fonts;

    /// Internal per-state-stack snapshot for VGContext.
    struct VGState
    {
        Mat4 transform = Mat4::Identity();
        Rect clipRect{};
        VGClipMode clipMode = VGClipMode::None;
        f32 opacity = 1.0f;
        i32 stencilRef = 0;
    };

    /// Main user-facing API for vector graphics drawing. Produces batched
    /// geometry (VGBatch) for rendering.
    class VGContext
    {
    public:
        explicit VGContext(fonts::IFontService* fontService = nullptr)
            : m_fontService(fontService)
        {
            m_stateStack.Reserve(16);

            // 1x1 white texture for solid-color draws (Textures[0] -> color passthrough).
            const u8 whitePixel[4] = { 255, 255, 255, 255 };
            m_whiteTexture = img::OwnedImageData(1, 1, img::PixelFormat::RGBA8, Span<const u8>(whitePixel, 4));
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
            m_currentTextureIndex = 0;
            m_commandStartIndex = 0;

            // Re-add the white texture at index 0 for solid color drawing.
            m_batch.textures.PushBack(&m_whiteTexture);
        }

        /// Tessellation tolerance (lower = smoother curves, more vertices).
        [[nodiscard]] f32 Tolerance() const { return m_tolerance; }
        void SetTolerance(f32 tolerance) { m_tolerance = tolerance; }

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

        void SetTransform(const Mat4& transform) { m_currentState.transform = transform; }
        [[nodiscard]] Mat4 GetTransform() const { return m_currentState.transform; }

        void Translate(f32 x, f32 y) { m_currentState.transform = Mat4::Translation(Vec3{ x, y, 0.0f }) * m_currentState.transform; }
        void Rotate(f32 radians)     { m_currentState.transform = Mat4::RotationZ(radians) * m_currentState.transform; }
        void Scale(f32 sx, f32 sy)   { m_currentState.transform = Mat4::Scale(Vec3{ sx, sy, 1.0f }) * m_currentState.transform; }
        void ResetTransform()        { m_currentState.transform = Mat4::Identity(); }

        // === Clipping ===

        void PushClipRect(Rect rect)
        {
            FlushCurrentCommand();
            m_clipStack.PushBack(m_currentState.clipRect);

            const Rect transformedRect = TransformRect(rect);
            if (m_currentState.clipRect.width > 0.0f && m_currentState.clipRect.height > 0.0f)
                m_currentState.clipRect = Rect::Intersect(m_currentState.clipRect, transformedRect);
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
                m_currentState.clipMode = (m_currentState.clipRect.width > 0.0f && m_currentState.clipRect.height > 0.0f)
                    ? VGClipMode::Scissor : VGClipMode::None;
            }
            else
            {
                m_currentState.clipRect = Rect{};
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

        // === Path Drawing ===

        /// Fill a path with a solid color.
        void FillPath(const Path& path, Color color, FillRule fillRule = FillRule::EvenOdd, bool antiAlias = true)
        {
            SetupForSolidDraw();
            const usize startVertex = m_batch.vertices.Size();
            const f32 scaledTolerance = GetScaledTolerance();
            FillTessellator::Tessellate(path, fillRule, ApplyOpacity(color), antiAlias, m_batch.vertices, m_batch.indices, scaledTolerance);
            TransformVertices(startVertex);
        }

        /// Fill a path with a fill style.
        void FillPath(const Path& path, const IVGFill& fill, FillRule fillRule = FillRule::EvenOdd, bool antiAlias = true)
        {
            SetupForSolidDraw();
            const usize startVertex = m_batch.vertices.Size();
            const f32 scaledTolerance = GetScaledTolerance();
            FillTessellator::TessellateWithFill(path, fillRule, fill, antiAlias, m_batch.vertices, m_batch.indices, scaledTolerance);
            ApplyOpacityToVertices(startVertex);
            TransformVertices(startVertex);
        }

        /// Stroke a path with a solid color.
        void StrokePath(const Path& path, Color color, StrokeStyle style, Span<const f32> dashPattern = {}, bool antiAlias = true)
        {
            SetupForSolidDraw();
            const f32 scaledTolerance = GetScaledTolerance();
            Array<FlattenedSubPath> subPaths;
            PathFlattener::Flatten(path, scaledTolerance, subPaths);

            const usize startVertex = m_batch.vertices.Size();
            const Color opColor = ApplyOpacity(color);

            for (usize s = 0; s < subPaths.Size(); ++s)
            {
                const FlattenedSubPath& subPath = subPaths[s];
                if (subPath.points.Size() < 2)
                    continue;
                StrokeTessellator::Tessellate(Span<const Vec2>(subPath.points.Data(), subPath.points.Size()),
                                              subPath.isClosed, style, dashPattern, antiAlias, opColor, m_batch.vertices, m_batch.indices);
            }

            TransformVertices(startVertex);
        }

        // === Convenience: Filled Shapes ===

        void FillRect(Rect rect, Color color)
        {
            PathBuilder pb;
            pb.MoveTo(rect.x, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y + rect.height);
            pb.LineTo(rect.x, rect.y + rect.height);
            pb.Close();
            FillPath(pb.ToPath(), color);
        }

        void FillRoundedRect(Rect rect, f32 radius, Color color) { FillRoundedRect(rect, CornerRadii(radius), color); }

        void FillRoundedRect(Rect rect, CornerRadii radii, Color color)
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

        void FillCircle(Vec2 center, f32 radius, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildCircle(center, radius, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillEllipse(Vec2 center, f32 rx, f32 ry, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildEllipse(center, rx, ry, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillRegularPolygon(Vec2 center, f32 radius, i32 sides, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildRegularPolygon(center, radius, sides, pb);
            FillPath(pb.ToPath(), color);
        }

        void FillStar(Vec2 center, f32 outerRadius, f32 innerRadius, i32 points, Color color)
        {
            PathBuilder pb;
            ShapeBuilder::BuildStar(center, outerRadius, innerRadius, points, pb);
            FillPath(pb.ToPath(), color);
        }

        // === Convenience: Stroked Shapes ===

        void StrokeRect(Rect rect, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            pb.MoveTo(rect.x, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y);
            pb.LineTo(rect.x + rect.width, rect.y + rect.height);
            pb.LineTo(rect.x, rect.y + rect.height);
            pb.Close();
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        void StrokeRoundedRect(Rect rect, f32 radius, Color color, f32 width = 1.0f) { StrokeRoundedRect(rect, CornerRadii(radius), color, width); }

        void StrokeRoundedRect(Rect rect, CornerRadii radii, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            ShapeBuilder::BuildRoundedRect(rect, radii, pb);
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        void StrokeCircle(Vec2 center, f32 radius, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            ShapeBuilder::BuildCircle(center, radius, pb);
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        void StrokeEllipse(Vec2 center, f32 rx, f32 ry, Color color, f32 width = 1.0f)
        {
            PathBuilder pb;
            ShapeBuilder::BuildEllipse(center, rx, ry, pb);
            StrokePath(pb.ToPath(), color, StrokeStyle(width));
        }

        // === UI Convenience ===

        void DrawLine(Vec2 a, Vec2 b, Color color, f32 thickness = 1.0f)
        {
            PathBuilder pb;
            pb.MoveTo(a.x, a.y);
            pb.LineTo(b.x, b.y);
            StrokePath(pb.ToPath(), color, StrokeStyle(thickness));
        }

        /// Draw a border rectangle with the stroke fully *inside* the rect bounds.
        void DrawBorderRect(Rect rect, Color color, f32 thickness = 1.0f)
        {
            const f32 halfThick = thickness * 0.5f;
            const Rect insetRect{ rect.x + halfThick, rect.y + halfThick, rect.width - thickness, rect.height - thickness };
            StrokeRect(insetRect, color, thickness);
        }

        void DrawBorderRoundedRect(Rect rect, f32 radius, Color color, f32 thickness = 1.0f) { DrawBorderRoundedRect(rect, CornerRadii(radius), color, thickness); }

        void DrawBorderRoundedRect(Rect rect, CornerRadii radii, Color color, f32 thickness = 1.0f)
        {
            const f32 halfThick = thickness * 0.5f;
            const Rect insetRect{ rect.x + halfThick, rect.y + halfThick, rect.width - thickness, rect.height - thickness };
            const CornerRadii insetRadii(
                Max(0.0f, radii.topLeft - halfThick),
                Max(0.0f, radii.topRight - halfThick),
                Max(0.0f, radii.bottomRight - halfThick),
                Max(0.0f, radii.bottomLeft - halfThick));
            StrokeRoundedRect(insetRect, insetRadii, color, thickness);
        }

        // === Immediate-Mode Path API ===

        void BeginPath() { m_currentPath.Clear(); }

        void MoveTo(f32 x, f32 y) { m_currentPath.MoveTo(x, y); }
        void MoveTo(Vec2 point) { m_currentPath.MoveTo(point); }
        void LineTo(f32 x, f32 y) { m_currentPath.LineTo(x, y); }
        void LineTo(Vec2 point) { m_currentPath.LineTo(point); }
        void QuadTo(f32 cx, f32 cy, f32 x, f32 y) { m_currentPath.QuadTo(cx, cy, x, y); }
        void QuadTo(Vec2 control, Vec2 end) { m_currentPath.QuadTo(control, end); }
        void CubicTo(f32 c1x, f32 c1y, f32 c2x, f32 c2y, f32 x, f32 y) { m_currentPath.CubicTo(c1x, c1y, c2x, c2y, x, y); }
        void CubicTo(Vec2 c1, Vec2 c2, Vec2 end) { m_currentPath.CubicTo(c1, c2, end); }
        void ArcTo(f32 rx, f32 ry, f32 xAxisRotation, bool largeArc, bool sweep, f32 x, f32 y) { m_currentPath.ArcTo(rx, ry, xAxisRotation, largeArc, sweep, x, y); }
        void ArcTo(f32 rx, f32 ry, f32 xAxisRotation, bool largeArc, bool sweep, Vec2 to) { m_currentPath.ArcTo(rx, ry, xAxisRotation, largeArc, sweep, to); }
        void ClosePath() { m_currentPath.Close(); }

        [[nodiscard]] Vec2 CurrentPoint() const { return m_currentPath.CurrentPoint(); }

        void Fill(Color color, FillRule fillRule = FillRule::EvenOdd, bool antiAlias = true)
        {
            if (m_currentPath.CommandCount() == 0) return;
            FillPath(m_currentPath.ToPath(), color, fillRule, antiAlias);
        }

        void Fill(const IVGFill& fill, FillRule fillRule = FillRule::EvenOdd, bool antiAlias = true)
        {
            if (m_currentPath.CommandCount() == 0) return;
            FillPath(m_currentPath.ToPath(), fill, fillRule, antiAlias);
        }

        void Stroke(Color color, StrokeStyle style, Span<const f32> dashPattern = {}, bool antiAlias = true)
        {
            if (m_currentPath.CommandCount() == 0) return;
            StrokePath(m_currentPath.ToPath(), color, style, dashPattern, antiAlias);
        }

        void Stroke(Color color, f32 thickness = 1.0f)
        {
            if (m_currentPath.CommandCount() == 0) return;
            StrokePath(m_currentPath.ToPath(), color, StrokeStyle(thickness));
        }

        // === Images ===

        void DrawImage(const img::ImageData* texture, Vec2 position)
        {
            if (texture == nullptr) return;
            DrawImage(texture,
                Rect{ position.x, position.y, static_cast<f32>(texture->Width()), static_cast<f32>(texture->Height()) },
                Rect{ 0.0f, 0.0f, static_cast<f32>(texture->Width()), static_cast<f32>(texture->Height()) },
                Color::White);
        }

        void DrawImage(const img::ImageData* texture, Vec2 position, Color tint)
        {
            if (texture == nullptr) return;
            DrawImage(texture,
                Rect{ position.x, position.y, static_cast<f32>(texture->Width()), static_cast<f32>(texture->Height()) },
                Rect{ 0.0f, 0.0f, static_cast<f32>(texture->Width()), static_cast<f32>(texture->Height()) },
                tint);
        }

        void DrawImage(const img::ImageData* texture, Rect destRect)
        {
            if (texture == nullptr) return;
            DrawImage(texture, destRect, Rect{ 0.0f, 0.0f, static_cast<f32>(texture->Width()), static_cast<f32>(texture->Height()) }, Color::White);
        }

        void DrawImage(const img::ImageData* texture, Rect destRect, Rect srcRect, Color tint)
        {
            if (texture == nullptr) return;

            const i32 textureIndex = GetOrAddTexture(texture);
            SetupForTextureDraw(textureIndex);

            const usize startVertex = m_batch.vertices.Size();
            EmitTexturedQuad(destRect, srcRect, texture->Width(), texture->Height(), ApplyOpacity(tint));
            TransformVertices(startVertex);
        }

        /// Draw a 9-slice image scaled to fit a destination rectangle.
        void DrawNineSlice(const img::ImageData* texture, Rect destRect, Rect srcRect, img::NineSlice slices, Color tint)
        {
            if (texture == nullptr) return;

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
            EmitTexturedQuad(Rect{ dstX0, dstY0, slices.left, slices.top },           Rect{ srcX0, srcY0, slices.left, slices.top },           tw, th, opTint);
            EmitTexturedQuad(Rect{ dstX1, dstY0, dstX2 - dstX1, slices.top },         Rect{ srcX1, srcY0, srcX2 - srcX1, slices.top },         tw, th, opTint);
            EmitTexturedQuad(Rect{ dstX2, dstY0, slices.right, slices.top },          Rect{ srcX2, srcY0, slices.right, slices.top },          tw, th, opTint);
            // Row 1 (middle).
            EmitTexturedQuad(Rect{ dstX0, dstY1, slices.left, dstY2 - dstY1 },        Rect{ srcX0, srcY1, slices.left, srcY2 - srcY1 },        tw, th, opTint);
            EmitTexturedQuad(Rect{ dstX1, dstY1, dstX2 - dstX1, dstY2 - dstY1 },      Rect{ srcX1, srcY1, srcX2 - srcX1, srcY2 - srcY1 },      tw, th, opTint);
            EmitTexturedQuad(Rect{ dstX2, dstY1, slices.right, dstY2 - dstY1 },       Rect{ srcX2, srcY1, slices.right, srcY2 - srcY1 },       tw, th, opTint);
            // Row 2 (bottom).
            EmitTexturedQuad(Rect{ dstX0, dstY2, slices.left, slices.bottom },        Rect{ srcX0, srcY2, slices.left, slices.bottom },        tw, th, opTint);
            EmitTexturedQuad(Rect{ dstX1, dstY2, dstX2 - dstX1, slices.bottom },      Rect{ srcX1, srcY2, srcX2 - srcX1, slices.bottom },      tw, th, opTint);
            EmitTexturedQuad(Rect{ dstX2, dstY2, slices.right, slices.bottom },       Rect{ srcX2, srcY2, slices.right, slices.bottom },       tw, th, opTint);

            TransformVertices(startVertex);
        }

        // === Text ===

        /// Draw text at a baseline position using a pre-rendered font atlas (low-level).
        void DrawText(StringView text, const fonts::IFontAtlas* atlas, const img::ImageData* atlasTexture, Vec2 position, Color color)
        {
            if (text.IsEmpty() || atlas == nullptr || atlasTexture == nullptr) return;

            const i32 textureIndex = GetOrAddTexture(atlasTexture);
            const bool isDF = atlas->Mode() == fonts::AtlasMode::DistanceField;
            if (isDF)
            {
                SetDrawMode(VGDrawMode::DistanceField);
                m_batch.dfPxRange = atlas->DistanceFieldRange();
                m_batch.dfAtlasW  = static_cast<f32>(atlas->Width());
                m_batch.dfAtlasH  = static_cast<f32>(atlas->Height());
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
        void DrawText(StringView text, const fonts::IFont* font, const fonts::IFontAtlas* atlas, const img::ImageData* atlasTexture,
                      Rect bounds, fonts::TextAlignment align, Color color)
        {
            if (text.IsEmpty() || font == nullptr) return;

            const f32 textWidth = font->MeasureString(text);
            f32 offsetX = bounds.x;
            switch (align)
            {
            case fonts::TextAlignment::Left:   offsetX = bounds.x; break;
            case fonts::TextAlignment::Center: offsetX = bounds.x + (bounds.width - textWidth) * 0.5f; break;
            case fonts::TextAlignment::Right:  offsetX = bounds.x + bounds.width - textWidth; break;
            }

            const fonts::FontMetrics fm = font->Metrics();
            const f32 offsetY = bounds.y + (bounds.height - fm.lineHeight) * 0.5f + fm.ascent;
            DrawText(text, atlas, atlasTexture, Vec2{ offsetX, offsetY }, color);
        }

        /// Draw text with horizontal and vertical alignment within bounds.
        void DrawText(StringView text, const fonts::IFont* font, const fonts::IFontAtlas* atlas, const img::ImageData* atlasTexture,
                      Rect bounds, fonts::TextAlignment hAlign, fonts::VerticalAlignment vAlign, Color color)
        {
            if (text.IsEmpty() || font == nullptr) return;

            const f32 textWidth = font->MeasureString(text);
            f32 offsetX = bounds.x;
            f32 offsetY = bounds.y;

            switch (hAlign)
            {
            case fonts::TextAlignment::Left:   offsetX = bounds.x; break;
            case fonts::TextAlignment::Center: offsetX = bounds.x + (bounds.width - textWidth) * 0.5f; break;
            case fonts::TextAlignment::Right:  offsetX = bounds.x + bounds.width - textWidth; break;
            }

            const fonts::FontMetrics fm = font->Metrics();
            switch (vAlign)
            {
            case fonts::VerticalAlignment::Top:      offsetY = bounds.y + fm.ascent; break;
            case fonts::VerticalAlignment::Middle:   offsetY = bounds.y + (bounds.height - fm.lineHeight) * 0.5f + fm.ascent; break;
            case fonts::VerticalAlignment::Bottom:   offsetY = bounds.y + bounds.height - fm.descent; break;
            case fonts::VerticalAlignment::Baseline: offsetY = bounds.y; break;
            }

            DrawText(text, atlas, atlasTexture, Vec2{ offsetX, offsetY }, color);
        }

        /// Convenience: draw text using a CachedFont (requires a FontService).
        void DrawText(StringView text, fonts::CachedFont* font, Vec2 position, Color color)
        {
            if (font == nullptr || m_fontService == nullptr) return;
            img::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr) return;
            DrawText(text, font->atlas, atlasTex, position, color);
        }

        /// Convenience: draw text using a CachedFont with alignment.
        void DrawText(StringView text, fonts::CachedFont* font, Rect bounds,
                      fonts::TextAlignment hAlign, fonts::VerticalAlignment vAlign, Color color)
        {
            if (font == nullptr || m_fontService == nullptr) return;
            img::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr) return;
            DrawText(text, font->font, font->atlas, atlasTex, bounds, hAlign, vAlign, color);
        }

        /// Draw pre-shaped glyphs at an offset (for scroll-offset text rendering).
        void DrawPositionedGlyphs(const Array<fonts::GlyphPosition>& positions, fonts::CachedFont* font, f32 offsetX, f32 offsetY, Color color)
        {
            if (positions.IsEmpty() || font == nullptr || m_fontService == nullptr) return;
            img::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr || font->atlas == nullptr) return;

            const i32 textureIndex = GetOrAddTexture(atlasTex);
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
        }

        /// Draw text with word wrapping. Position is the top-left of the text block.
        void DrawTextWrapped(StringView text, fonts::CachedFont* font, Vec2 position, f32 maxWidth, Color color,
                             fonts::TextAlignment hAlign = fonts::TextAlignment::Left)
        {
            if (text.IsEmpty() || font == nullptr || font->shaper == nullptr || m_fontService == nullptr) return;
            img::ImageData* atlasTex = m_fontService->GetAtlasTexture(font);
            if (atlasTex == nullptr) return;

            Array<fonts::GlyphPosition> positions;
            f32 totalHeight = 0.0f;
            if (!font->shaper->ShapeTextWrapped(*font->font, text, maxWidth, positions, totalHeight).IsOk())
                return;

            if (hAlign != fonts::TextAlignment::Left)
                ApplyLineAlignment(positions, maxWidth, hAlign);

            DrawPositionedGlyphs(positions, font, position.x, position.y + font->font->Metrics().ascent, color);
        }

        void DrawTextWrapped(StringView text, fonts::CachedFont* font, Rect bounds, Color color,
                             fonts::TextAlignment hAlign = fonts::TextAlignment::Left)
        {
            DrawTextWrapped(text, font, Vec2{ bounds.x, bounds.y }, bounds.width, color, hAlign);
        }

        /// Measure wrapped text without drawing. Returns total height (0 if no shaper).
        [[nodiscard]] f32 MeasureTextWrapped(StringView text, fonts::CachedFont* font, f32 maxWidth)
        {
            if (text.IsEmpty() || font == nullptr || font->shaper == nullptr) return 0.0f;

            Array<fonts::GlyphPosition> positions;
            f32 totalHeight = 0.0f;
            if (!font->shaper->ShapeTextWrapped(*font->font, text, maxWidth, positions, totalHeight).IsOk())
                return 0.0f;
            return totalHeight;
        }

        /// Draw text using the default font at the given pixel size (requires a FontService).
        void DrawText(StringView text, f32 fontSize, Vec2 position, Color color)
        {
            if (text.IsEmpty() || m_fontService == nullptr) return;
            fonts::CachedFont* font = m_fontService->GetFont(fontSize);
            if (font == nullptr) return;
            DrawText(text, font, position, color);
        }

        /// Fill a polygon defined by a span of points.
        void FillPolygon(Span<const Vec2> points, Color color)
        {
            if (points.Size() < 3) return;

            BeginPath();
            MoveTo(points[0]);
            for (usize i = 1; i < points.Size(); ++i)
                LineTo(points[i]);
            ClosePath();
            Fill(color);
        }

        /// Measure the width and line height of a string in pixels.
        [[nodiscard]] Vec2 MeasureText(StringView text, const fonts::IFont* font)
        {
            if (font == nullptr) return Vec2::Zero;
            return Vec2{ font->MeasureString(text), font->Metrics().lineHeight };
        }

        /// Measure just the pixel width of a string.
        [[nodiscard]] f32 MeasureTextWidth(StringView text, const fonts::IFont* font)
        {
            if (font == nullptr) return 0.0f;
            return font->MeasureString(text);
        }

    private:
        /// Applies horizontal alignment offsets to shaped glyph positions per line.
        static void ApplyLineAlignment(Array<fonts::GlyphPosition>& positions, f32 maxWidth, fonts::TextAlignment align)
        {
            if (positions.IsEmpty()) return;

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

            m_batch.vertices.PushBack(VGVertex(Vec2{ quad.x0, quad.y0 }, Vec2{ quad.u0, quad.v0 }, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Vec2{ quad.x1, quad.y0 }, Vec2{ quad.u1, quad.v0 }, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Vec2{ quad.x1, quad.y1 }, Vec2{ quad.u1, quad.v1 }, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Vec2{ quad.x0, quad.y1 }, Vec2{ quad.u0, quad.v1 }, color, 1.0f));

            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 1);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 3);
        }

        /// Emit a textured quad into the batch in untransformed coordinates (coverage 1.0).
        void EmitTexturedQuad(Rect destRect, Rect srcRect, u32 texWidth, u32 texHeight, Color color)
        {
            if (destRect.width <= 0.0f || destRect.height <= 0.0f)
                return;

            const u32 baseIndex = static_cast<u32>(m_batch.vertices.Size());

            const f32 u0 = srcRect.x / static_cast<f32>(texWidth);
            const f32 v0 = srcRect.y / static_cast<f32>(texHeight);
            const f32 u1 = (srcRect.x + srcRect.width) / static_cast<f32>(texWidth);
            const f32 v1 = (srcRect.y + srcRect.height) / static_cast<f32>(texHeight);

            m_batch.vertices.PushBack(VGVertex(Vec2{ destRect.x, destRect.y }, Vec2{ u0, v0 }, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Vec2{ destRect.x + destRect.width, destRect.y }, Vec2{ u1, v0 }, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Vec2{ destRect.x + destRect.width, destRect.y + destRect.height }, Vec2{ u1, v1 }, color, 1.0f));
            m_batch.vertices.PushBack(VGVertex(Vec2{ destRect.x, destRect.y + destRect.height }, Vec2{ u0, v1 }, color, 1.0f));

            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 1);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 0);
            m_batch.indices.PushBack(baseIndex + 2);
            m_batch.indices.PushBack(baseIndex + 3);
        }

        /// Look up a texture in the batch or append it. Index 0 is the white texture.
        i32 GetOrAddTexture(const img::ImageData* tex)
        {
            if (tex == nullptr) return 0;
            for (usize i = 0; i < m_batch.textures.Size(); ++i)
                if (m_batch.textures[i] == tex)
                    return static_cast<i32>(i);
            m_batch.textures.PushBack(tex);
            return static_cast<i32>(m_batch.textures.Size() - 1);
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

        void SetDrawMode(VGDrawMode mode)
        {
            if (m_currentDrawMode != mode)
            {
                FlushCurrentCommand();
                m_currentDrawMode = mode;
            }
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

                m_batch.commands.PushBack(cmd);
                m_commandStartIndex = static_cast<i32>(m_batch.indices.Size());
            }
        }

        /// Tolerance adjusted for current transform scale (tighter when scaled up).
        [[nodiscard]] f32 GetScaledTolerance() const
        {
            if (m_currentState.transform == Mat4::Identity())
                return m_tolerance;

            const f32 sx = Length(Vec2{ m_currentState.transform(0, 0), m_currentState.transform(0, 1) });
            const f32 sy = Length(Vec2{ m_currentState.transform(1, 0), m_currentState.transform(1, 1) });
            const f32 scale = Max(sx, sy);

            if (scale > 0.0001f)
                return m_tolerance / scale;
            return m_tolerance;
        }

        [[nodiscard]] Vec2 TransformPoint(Vec2 point) const
        {
            if (m_currentState.transform == Mat4::Identity())
                return point;
            return TransformPoint2D(point, m_currentState.transform);
        }

        void TransformVertices(usize startVertex)
        {
            if (m_currentState.transform == Mat4::Identity())
                return;

            for (usize i = startVertex; i < m_batch.vertices.Size(); ++i)
                m_batch.vertices[i].position = TransformPoint2D(m_batch.vertices[i].position, m_currentState.transform);
        }

        [[nodiscard]] Color ApplyOpacity(Color color) const
        {
            if (m_currentState.opacity >= 1.0f)
                return color;
            return Color{ color.r, color.g, color.b, color.a * m_currentState.opacity };
        }

        void ApplyOpacityToVertices(usize startVertex)
        {
            if (m_currentState.opacity >= 1.0f)
                return;
            // Vertices store packed Color32; lift to float, apply, re-pack.
            for (usize i = startVertex; i < m_batch.vertices.Size(); ++i)
                m_batch.vertices[i].color = ToColor32(ApplyOpacity(ToColor(m_batch.vertices[i].color)));
        }

        [[nodiscard]] Rect TransformRect(Rect rect) const
        {
            if (m_currentState.transform == Mat4::Identity())
                return rect;

            const Vec2 topLeft = TransformPoint(Vec2{ rect.x, rect.y });
            const Vec2 bottomRight = TransformPoint(Vec2{ rect.x + rect.width, rect.y + rect.height });
            return Rect{ topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y };
        }

        VGBatch m_batch;
        img::OwnedImageData m_whiteTexture;
        PathBuilder m_currentPath;
        fonts::IFontService* m_fontService = nullptr;

        Array<VGState> m_stateStack;
        VGState m_currentState;

        Array<Rect> m_clipStack;
        Array<f32> m_opacityStack;

        VGBlendMode m_currentBlendMode = VGBlendMode::Normal;
        VGDrawMode m_currentDrawMode = VGDrawMode::Default;
        i32 m_currentTextureIndex = 0;
        i32 m_commandStartIndex = 0;
        f32 m_tolerance = 0.05f;
    };
}
