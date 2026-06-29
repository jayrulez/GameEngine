// VG Sandbox — faithful port of Sedulous Samples/VG/VGSandbox. A NanoVG-style
// demo exercising the whole VG stack (raptor.vg + .renderer + .svg + fonts +
// image): line widths/caps/joins, animated eyes, an HSL color wheel, an area
// graph, scissor clipping, image draws, text, UI convenience primitives,
// immediate-mode paths, and SVG rendering. Window/device/swapchain via the
// sample framework; per frame: VGContext -> VGBatch -> VGRenderer -> RHI.

#include <new>
#include <cstdio>

import raptor.core;
import raptor.rhi;
import raptor.rhi.vk;
import raptor.shaders;
import raptor.samples.framework;
import raptor.image;
import raptor.fonts;
import raptor.fonts.ttf;
import raptor.fonts.df;
import raptor.fonts.df.baker;
import raptor.vg;
import raptor.vg.renderer;
import raptor.vg.svg;

using namespace raptor::core;
namespace sf = raptor::samples::framework;
namespace rhi = raptor::rhi;
namespace ds = raptor::shaders;
namespace img = raptor::image;
namespace fonts = raptor::fonts;
namespace vg = raptor::vg;
namespace vgr = raptor::vg::renderer;
namespace svg = raptor::vg::svg;

namespace
{
    constexpr const char8_t kVertSrc[] = u8R"(
#pragma pack_matrix(row_major)
cbuffer VGUniforms : register(b0) { float4x4 Projection; float DFPxRange; float DFAtlasW; float DFAtlasH; float _pad; };
struct VSInput { float2 Position:TEXCOORD0; float2 TexCoord:TEXCOORD1; float4 Color:TEXCOORD2; float Coverage:TEXCOORD3; };
struct VSOutput { float4 Position:SV_Position; float2 TexCoord:TEXCOORD0; float4 Color:COLOR0; float Coverage:COVERAGE; };
VSOutput main(VSInput input) {
    VSOutput o;
    o.Position = mul(float4(input.Position, 0.0, 1.0), Projection);
    o.TexCoord = input.TexCoord; o.Color = input.Color; o.Coverage = input.Coverage;
    return o;
}
)";

    constexpr const char8_t kFragSrc[] = u8R"(
struct PSInput { float4 Position:SV_Position; float2 TexCoord:TEXCOORD0; float4 Color:COLOR0; float Coverage:COVERAGE; };
Texture2D VGTexture : register(t0);
SamplerState VGSampler : register(s0);
float4 main(PSInput input) : SV_Target {
    float4 texColor = VGTexture.Sample(VGSampler, input.TexCoord);
    float4 result = texColor * input.Color;
    result.a *= input.Coverage;
    return result;
}
)";

    constexpr const char8_t kDFFragSrc[] = u8R"(
#pragma pack_matrix(row_major)
struct PSInput { float4 Position:SV_Position; float2 TexCoord:TEXCOORD0; float4 Color:COLOR0; float Coverage:COVERAGE; };
cbuffer VGUniforms : register(b0) { float4x4 Projection; float DFPxRange; float DFAtlasW; float DFAtlasH; float _pad; };
Texture2D VGTexture : register(t0);
SamplerState VGSampler : register(s0);
float Median(float r, float g, float b) { return max(min(r,g), min(max(r,g), b)); }
float4 main(PSInput input) : SV_Target {
    float4 msd = VGTexture.Sample(VGSampler, input.TexCoord);
    float sd = Median(msd.r, msd.g, msd.b);
    float opacity = clamp(4.0 * (sd - 0.5) + 0.5, 0.0, 1.0);

    float4 result = input.Color;
    result.a *= opacity;
    return result;
}
)";

#ifndef RAPTOR_VG_FONT_PATH
#define RAPTOR_VG_FONT_PATH ""
#endif

    // Gradient stops take the engine's float Color; UI colors are byte Color32.
    inline Color GC(u8 r, u8 g, u8 b, u8 a = 255) { return ToColor(Color32{ r, g, b, a }); }
}

class VGSandbox : public sf::SampleApp
{
public:
    VGSandbox() { m_width = 1000; m_height = 720; } // match Sedulous VGSandbox layout
    StringView Title() const override { return u8"VG Sandbox"; }
    u32 BufferCount() const override { return kFrames; }

protected:
    Status OnInit() override;
    void   OnRender() override;
    void   OnShutdown() override;

private:
    static constexpr u32 kFrames = 2;

    void DrawScene(vg::VGContext& vgc, f32 w, f32 h, f32 t);
    void DrawLineWidths(vg::VGContext& vgc, f32 x, f32 y);
    void DrawLineCaps(vg::VGContext& vgc, f32 x, f32 y);
    void DrawEyes(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t);
    void DrawLineJoins(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t);
    void DrawColorWheel(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t);
    void DrawGraph(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t);
    void DrawScissor(vg::VGContext& vgc, f32 x, f32 y, f32 t);
    void DrawImages(vg::VGContext& vgc, f32 x, f32 y, f32 t);
    void DrawTextDemo(vg::VGContext& vgc, f32 x, f32 y, f32 t);
    void DrawUIConvenience(vg::VGContext& vgc, f32 x, f32 y);
    void DrawImmediatePath(vg::VGContext& vgc, f32 x, f32 y, f32 t);
    void DrawSVGDemo(vg::VGContext& vgc, f32 x, f32 y);
    void DrawDFTextDemo(vg::VGContext& vgc, f32 x, f32 y);

    void LoadFontSize(StringView path, f32 pixelHeight);
    static Color HSLToColor(f32 h, f32 s, f32 l);
    static f32 HueToRGB(f32 p, f32 q, f32 t);
    [[nodiscard]] bool HasFonts() const { return !StringView(reinterpret_cast<const utf8char*>(RAPTOR_VG_FONT_PATH)).IsEmpty(); }

    ds::Compiler*      m_compiler = nullptr;
    rhi::ShaderModule* m_vs = nullptr;
    rhi::ShaderModule* m_fs = nullptr;
    rhi::ShaderModule* m_dfFs = nullptr;
    rhi::CommandPool*  m_pool = nullptr;
    rhi::Fence*        m_fence = nullptr;
    u64                m_fenceVal = 0;
    u32                m_frameIndex = 0;

    UniquePtr<fonts::TrueTypeFontService> m_fontService;
    UniquePtr<vg::VGContext> m_vg;
    vgr::VGRenderer    m_renderer;
    img::OwnedImageData m_checker;

    fonts::CachedFont* m_fontSmall = nullptr;
    fonts::CachedFont* m_fontMedium = nullptr;
    fonts::CachedFont* m_fontLarge = nullptr;
    fonts::CachedFont* m_fontDF = nullptr;

    svg::SVGDocument m_badge; bool m_hasBadge = false;
    svg::SVGDocument m_icon;  bool m_hasIcon = false;
};

Status VGSandbox::OnInit()
{
    if (ds::createCompiler(ds::CompilerDesc{}, m_compiler) != ErrorCode::Ok) return ErrorCode::Unknown;
    if (sf::CompileToModule(m_compiler, m_device, kVertSrc, ds::ShaderStage::Vertex,   u8"main", u8"vg.vert", m_vs) != ErrorCode::Ok) return ErrorCode::Unknown;
    if (sf::CompileToModule(m_compiler, m_device, kFragSrc, ds::ShaderStage::Fragment, u8"main", u8"vg.frag", m_fs) != ErrorCode::Ok) return ErrorCode::Unknown;
    if (sf::CompileToModule(m_compiler, m_device, kDFFragSrc, ds::ShaderStage::Fragment, u8"main", u8"vg_df.frag", m_dfFs) != ErrorCode::Ok) return ErrorCode::Unknown;

    if (!m_renderer.Initialize(*m_device, *m_vs, *m_fs, m_swapChain->Format(), static_cast<i32>(kFrames), m_dfFs).IsOk())
        return ErrorCode::Unknown;

    if (m_device->CreateCommandPool(rhi::QueueType::Graphics, m_pool) != ErrorCode::Ok) return ErrorCode::Unknown;
    if (m_device->CreateFence(0, m_fence) != ErrorCode::Ok) return ErrorCode::Unknown;

    // Fonts (Roboto at three sizes). Text is skipped if the font isn't available.
    m_fontService = MakeUnique<fonts::TrueTypeFontService>(DefaultAllocator());
    if (HasFonts())
    {
        const StringView fontPath(reinterpret_cast<const utf8char*>(RAPTOR_VG_FONT_PATH));
        LoadFontSize(fontPath, 14.0f);
        LoadFontSize(fontPath, 20.0f);
        LoadFontSize(fontPath, 36.0f);
        m_fontSmall  = m_fontService->GetFont(u8"Roboto", 14.0f);
        m_fontMedium = m_fontService->GetFont(u8"Roboto", 20.0f);
        m_fontLarge  = m_fontService->GetFont(u8"Roboto", 36.0f);

        // Distance-field font (single atlas works at any size).
        fonts::DFFonts::Initialize();
        fonts::FontLoadOptions dfOpts = fonts::FontLoadOptions::DistanceField();
        dfOpts.pixelHeight = 48.0f;
        dfOpts.atlasWidth = 1024; dfOpts.atlasHeight = 1024;
        (void)m_fontService->LoadFont(u8"RobotoDF", fontPath, dfOpts);
        m_fontDF = m_fontService->GetFont(u8"RobotoDF", 48.0f);
    }

    m_vg = MakeUnique<vg::VGContext>(DefaultAllocator(), m_fontService.Get());

    // 128x128 checkerboard image for the DrawImage demos.
    {
        img::Image checker = img::Image::CreateCheckerboard(128, Color32{ 230, 230, 230, 255 }, Color32{ 60, 60, 70, 255 }, 16);
        const Span<const u8> px = checker.PixelData();
        Array<u8> copy; copy.Resize(px.Size());
        if (px.Size() > 0) MemCopy(copy.Data(), px.Data(), px.Size());
        m_checker = img::OwnedImageData(checker.Width(), checker.Height(), img::PixelFormat::RGBA8, Move(copy));
    }

    // SVG badge + star icon.
    {
        Result<svg::SVGDocument> r = svg::SVGLoader::Load(
            u8"<svg viewBox=\"0 0 100 100\">"
            u8"<circle cx=\"50\" cy=\"50\" r=\"45\" fill=\"#2A6BC0\" stroke=\"#1A4A90\" stroke-width=\"3\"/>"
            u8"<circle cx=\"50\" cy=\"50\" r=\"30\" fill=\"none\" stroke=\"#4A9AFF\" stroke-width=\"1.5\" opacity=\"0.6\"/>"
            u8"<text x=\"50\" y=\"58\" text-anchor=\"middle\" font-size=\"28\" font-weight=\"bold\" fill=\"#FFFFFF\">VG</text>"
            u8"</svg>");
        if (r.HasValue()) { m_badge = Move(r.Value()); m_hasBadge = true; }
    }
    {
        Result<svg::SVGDocument> r = svg::SVGLoader::Load(
            u8"<svg viewBox=\"0 0 24 24\">"
            u8"<path d=\"M12 2L15.09 8.26L22 9.27L17 14.14L18.18 21.02L12 17.77L5.82 21.02L7 14.14L2 9.27L8.91 8.26L12 2Z\" fill=\"#FFD700\" stroke=\"#B8960F\" stroke-width=\"0.8\"/>"
            u8"<text x=\"12\" y=\"14\" text-anchor=\"middle\" font-size=\"6\" fill=\"#8B6914\">5</text>"
            u8"</svg>");
        if (r.HasValue()) { m_icon = Move(r.Value()); m_hasIcon = true; }
    }

    return ErrorCode::Ok;
}

void VGSandbox::LoadFontSize(StringView path, f32 pixelHeight)
{
    fonts::FontLoadOptions options = fonts::FontLoadOptions::ExtendedLatin();
    options.pixelHeight = pixelHeight;
    (void)m_fontService->LoadFont(u8"Roboto", path, options);
}

void VGSandbox::DrawScene(vg::VGContext& vgc, f32 w, f32 h, f32 t)
{
    DrawLineWidths(vgc, 10, 10);
    DrawLineCaps(vgc, 10, 230);
    DrawEyes(vgc, w - 250, 10, 150, 100, t);
    DrawLineJoins(vgc, 10, 290, 500, 50, t);
    DrawColorWheel(vgc, w - 280, 120, 250, 250, t);
    DrawGraph(vgc, 0, h - 180, w, 180, t);
    DrawScissor(vgc, 20, h - 220, t);
    DrawImages(vgc, 150, 20, t);
    DrawTextDemo(vgc, 150, 170, t);
    DrawUIConvenience(vgc, 150, 340);
    DrawImmediatePath(vgc, 150, 410, t);
    DrawSVGDemo(vgc, 150, 470);
    DrawDFTextDemo(vgc, w - 280, 390);
}

void VGSandbox::DrawLineWidths(vg::VGContext& vgc, f32 x, f32 y)
{
    for (i32 i = 0; i < 20; ++i)
    {
        const f32 lw = (static_cast<f32>(i) + 0.5f) * 0.1f;
        vg::PathBuilder pb;
        pb.MoveTo(x, y + static_cast<f32>(i) * 10.0f);
        pb.LineTo(x + 100.0f, y + static_cast<f32>(i) * 10.0f);
        vgc.StrokePath(pb.ToPath(), GC( 255, 255, 255, 255 ), vg::StrokeStyle(lw));
    }
}

void VGSandbox::DrawLineCaps(vg::VGContext& vgc, f32 x, f32 y)
{
    const vg::VGLineCap caps[3] = { vg::VGLineCap::Butt, vg::VGLineCap::Round, vg::VGLineCap::Square };
    for (i32 i = 0; i < 3; ++i)
    {
        const f32 ly = y + static_cast<f32>(i) * 14.0f;
        vg::StrokeStyle style(8.0f, caps[i], vg::VGLineJoin::Miter);

        vg::PathBuilder pb;
        pb.MoveTo(x, ly); pb.LineTo(x + 80.0f, ly);
        vgc.StrokePath(pb.ToPath(), GC( 255, 255, 255, 160 ), style);

        vg::PathBuilder rpb;
        rpb.MoveTo(x, ly); rpb.LineTo(x + 80.0f, ly);
        vgc.StrokePath(rpb.ToPath(), GC( 0, 192, 255, 255 ), vg::StrokeStyle(1.0f));
    }
}

void VGSandbox::DrawEyes(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t)
{
    const f32 ex = w * 0.23f;
    const f32 ey = h * 0.5f;
    const f32 br = Min(ex, ey) * 0.5f;
    const f32 lx = x + w * 0.5f + Cos(t * 0.8f) * w * 0.3f;
    const f32 ly = y + h * 0.5f + Sin(t * 0.6f) * h * 0.4f;

    for (i32 side = 0; side < 2; ++side)
    {
        const f32 cx = x + ex + static_cast<f32>(side) * (w - ex * 2.0f);
        const f32 cy = y + ey;

        // Shadow
        {
            vg::PathBuilder pb;
            vg::ShapeBuilder::BuildEllipse(Vec2{ cx + 1.0f, cy + 2.0f }, ex + 1.0f, ey + 1.0f, pb);
            vg::VGRadialGradientFill fill(Vec2{ cx, cy }, Max(ex, ey));
            fill.AddStop(0.0f, GC(0, 0, 0, 40));
            fill.AddStop(1.0f, GC(0, 0, 0, 0));
            vgc.FillPath(pb.ToPath(), fill);
        }
        // White
        {
            vg::PathBuilder pb;
            vg::ShapeBuilder::BuildEllipse(Vec2{ cx, cy }, ex, ey, pb);
            vg::VGLinearGradientFill fill(Vec2{ cx, cy - ey * 0.5f }, Vec2{ cx, cy + ey * 0.5f });
            fill.AddStop(0.0f, GC(255, 255, 255, 255));
            fill.AddStop(1.0f, GC(220, 220, 220, 255));
            vgc.FillPath(pb.ToPath(), fill);
        }
        // Iris (tracks the look-at point)
        {
            f32 dx = lx - cx, dy = ly - cy;
            const f32 d = Sqrt(dx * dx + dy * dy);
            if (d > 1.0f) { dx /= d; dy /= d; }
            const f32 irisX = cx + dx * (ex - br) * 0.4f;
            const f32 irisY = cy + dy * (ey - br) * 0.5f;
            {
                vg::PathBuilder pb;
                vg::ShapeBuilder::BuildCircle(Vec2{ irisX, irisY }, br, pb);
                vg::VGRadialGradientFill fill(Vec2{ irisX, irisY }, br);
                fill.AddStop(0.0f, GC(60, 90, 160, 255));
                fill.AddStop(0.7f, GC(30, 50, 90, 255));
                fill.AddStop(1.0f, GC(20, 30, 60, 255));
                vgc.FillPath(pb.ToPath(), fill);
            }
            vgc.FillCircle(Vec2{ irisX, irisY }, br * 0.45f, GC( 20, 20, 20, 255 ));
            vgc.FillCircle(Vec2{ irisX - br * 0.25f, irisY - br * 0.2f }, br * 0.15f, GC( 255, 255, 255, 200 ));
        }
    }
}

void VGSandbox::DrawLineJoins(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t)
{
    const f32 s = 30.0f;
    const vg::VGLineJoin joins[3] = { vg::VGLineJoin::Miter, vg::VGLineJoin::Round, vg::VGLineJoin::Bevel };
    const vg::VGLineCap caps[3] = { vg::VGLineCap::Butt, vg::VGLineCap::Round, vg::VGLineCap::Square };

    for (i32 i = 0; i < 3; ++i)
        for (i32 j = 0; j < 3; ++j)
        {
            const f32 fx = x + (static_cast<f32>(i) * 3.0f + static_cast<f32>(j)) * (w / 9.0f) + s * 0.5f;
            const f32 fy = y + h * 0.5f;

            f32 pts[8];
            pts[0] = -s * 0.25f + Cos(t * 0.3f) * s * 0.5f;
            pts[1] = Sin(t * 0.3f) * s * 0.5f;
            pts[2] = -s * 0.25f; pts[3] = 0.0f;
            pts[4] = s * 0.25f;  pts[5] = 0.0f;
            pts[6] = s * 0.25f + Cos(-t * 0.3f) * s * 0.5f;
            pts[7] = Sin(-t * 0.3f) * s * 0.5f;

            vg::PathBuilder pb;
            pb.MoveTo(fx + pts[0], fy + pts[1]);
            pb.LineTo(fx + pts[2], fy + pts[3]);
            pb.LineTo(fx + pts[4], fy + pts[5]);
            pb.LineTo(fx + pts[6], fy + pts[7]);
            const vg::Path path = pb.ToPath();

            vgc.StrokePath(path, GC( 0, 0, 0, 160 ), vg::StrokeStyle(s * 0.3f, caps[j], joins[i]));
            vgc.StrokePath(path, GC( 0, 192, 255, 255 ), vg::StrokeStyle(1.0f, vg::VGLineCap::Butt, vg::VGLineJoin::Miter));
        }
}

void VGSandbox::DrawColorWheel(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t)
{
    const f32 cx = x + w * 0.5f;
    const f32 cy = y + h * 0.5f;
    const f32 r1 = Min(w, h) * 0.5f - 5.0f;
    const f32 r0 = r1 - 20.0f;
    const f32 hue = Sin(t * 0.12f) * kTwoPi;

    const i32 segCount = 36;
    const f32 segAngle = kTwoPi / static_cast<f32>(segCount);
    for (i32 i = 0; i < segCount; ++i)
    {
        const f32 a0 = static_cast<f32>(i) * segAngle - segAngle * 0.5f;
        const f32 a1 = a0 + segAngle;

        vg::PathBuilder pb;
        const i32 steps = 4;
        for (i32 s = 0; s <= steps; ++s)
        {
            const f32 a = a0 + (a1 - a0) * (static_cast<f32>(s) / static_cast<f32>(steps));
            const f32 px = cx + Cos(a) * r1, py = cy + Sin(a) * r1;
            if (s == 0) pb.MoveTo(px, py); else pb.LineTo(px, py);
        }
        for (i32 s = steps; s >= 0; --s)
        {
            const f32 a = a0 + (a1 - a0) * (static_cast<f32>(s) / static_cast<f32>(steps));
            pb.LineTo(cx + Cos(a) * r0, cy + Sin(a) * r0);
        }
        pb.Close();

        const Color c0 = HSLToColor(a0 / kTwoPi, 1.0f, 0.5f);
        const Color c1 = HSLToColor(a1 / kTwoPi, 1.0f, 0.5f);
        vg::VGLinearGradientFill fill(
            Vec2{ cx + Cos(a0) * (r0 + r1) * 0.5f, cy + Sin(a0) * (r0 + r1) * 0.5f },
            Vec2{ cx + Cos(a1) * (r0 + r1) * 0.5f, cy + Sin(a1) * (r0 + r1) * 0.5f });
        fill.AddStop(0.0f, c0);
        fill.AddStop(1.0f, c1);
        vgc.FillPath(pb.ToPath(), fill);
    }

    // Selector indicator on the ring.
    {
        vgc.PushState();
        vgc.Translate(cx, cy);
        vgc.Rotate(hue);
        vg::PathBuilder pb;
        pb.MoveTo(r0 - 1.0f, -3.0f); pb.LineTo(r1 + 1.0f, -3.0f);
        pb.LineTo(r1 + 1.0f, 3.0f);  pb.LineTo(r0 - 1.0f, 3.0f);
        pb.Close();
        vgc.StrokePath(pb.ToPath(), GC( 255, 255, 255, 192 ), vg::StrokeStyle(2.0f));
        vgc.PopState();
    }

    // Center triangle.
    {
        const f32 r = r0 - 6.0f;
        const f32 ax = cx + Cos(hue + kTwoPi / 3.0f) * r, ay = cy + Sin(hue + kTwoPi / 3.0f) * r;
        const f32 bx = cx + Cos(hue - kTwoPi / 3.0f) * r, by = cy + Sin(hue - kTwoPi / 3.0f) * r;
        const f32 cxx = cx + Cos(hue) * r, cyy = cy + Sin(hue) * r;
        const Color hueColor = HSLToColor(hue / kTwoPi, 1.0f, 0.5f);

        vg::PathBuilder pb;
        pb.MoveTo(ax, ay); pb.LineTo(bx, by); pb.LineTo(cxx, cyy); pb.Close();
        const vg::Path path = pb.ToPath();

        vg::VGLinearGradientFill fill1(Vec2{ ax, ay }, Vec2{ cxx, cyy });
        fill1.AddStop(0.0f, GC(255, 255, 255, 255));
        fill1.AddStop(1.0f, hueColor);
        vgc.FillPath(path, fill1);

        vg::VGLinearGradientFill fill2(Vec2{ (ax + bx) * 0.5f, (ay + by) * 0.5f }, Vec2{ cxx, cyy });
        fill2.AddStop(0.0f, GC(0, 0, 0, 128));
        fill2.AddStop(1.0f, GC(0, 0, 0, 0));
        vgc.FillPath(path, fill2);

        vgc.StrokePath(path, GC( 0, 0, 0, 64 ), vg::StrokeStyle(2.0f));

        const f32 selX = ax + (cxx - ax) * 0.3f + (bx - ax) * 0.4f;
        const f32 selY = ay + (cyy - ay) * 0.3f + (by - ay) * 0.4f;
        vgc.StrokeCircle(Vec2{ selX, selY }, 5.0f, GC( 255, 255, 255, 192 ), 2.0f);
        vgc.FillCircle(Vec2{ selX, selY }, 3.5f, hueColor);
    }
}

void VGSandbox::DrawGraph(vg::VGContext& vgc, f32 x, f32 y, f32 w, f32 h, f32 t)
{
    f32 samples[6];
    for (i32 i = 0; i < 6; ++i)
    {
        const f32 raw = (1.0f + Sin(t * 1.2345f + static_cast<f32>(i) * 0.33457f + static_cast<f32>(i) * static_cast<f32>(i) * 0.12f)
            + Sin(t * 0.68363f + static_cast<f32>(i) * 1.3f)
            + Sin(t * 1.1642f + static_cast<f32>(i) * static_cast<f32>(i) * 0.54f)) * 0.25f;
        // Clamp >= 0. The raw value ranges [-0.5, 1.0]; a negative sample pushes the
        // curve point below the baseline (y+h), so the area-fill "ribbon" self-
        // intersects and ear-clipping emits sliver triangles — the stray line
        // artifacts seen in the original Sedulous sample. (Latent data-gen bug.)
        samples[i] = Max(0.0f, raw);
    }

    const f32 dx = w / 5.0f;

    // Filled area under curve.
    {
        vg::PathBuilder pb;
        pb.MoveTo(x, y + h);
        for (i32 i = 0; i < 6; ++i)
        {
            const f32 sx = x + static_cast<f32>(i) * dx;
            const f32 sy = y + h * (1.0f - samples[i] * 0.8f);
            if (i == 0) pb.LineTo(sx, sy);
            else
            {
                const f32 px = x + static_cast<f32>(i - 1) * dx;
                const f32 py = y + h * (1.0f - samples[i - 1] * 0.8f);
                pb.CubicTo(px + dx * 0.5f, py, sx - dx * 0.5f, sy, sx, sy);
            }
        }
        pb.LineTo(x + w, y + h);
        pb.Close();
        vg::VGLinearGradientFill fill(Vec2{ x, y }, Vec2{ x, y + h });
        fill.AddStop(0.0f, GC(0, 160, 192, 128));
        fill.AddStop(1.0f, GC(0, 160, 192, 16));
        vgc.FillPath(pb.ToPath(), fill);
    }

    // Stroke the curve.
    {
        vg::PathBuilder pb;
        for (i32 i = 0; i < 6; ++i)
        {
            const f32 sx = x + static_cast<f32>(i) * dx;
            const f32 sy = y + h * (1.0f - samples[i] * 0.8f);
            if (i == 0) pb.MoveTo(sx, sy);
            else
            {
                const f32 px = x + static_cast<f32>(i - 1) * dx;
                const f32 py = y + h * (1.0f - samples[i - 1] * 0.8f);
                pb.CubicTo(px + dx * 0.5f, py, sx - dx * 0.5f, sy, sx, sy);
            }
        }
        const vg::Path path = pb.ToPath();

        vgc.PushState();
        vgc.Translate(0, 2);
        vgc.StrokePath(path, GC( 0, 0, 0, 32 ), vg::StrokeStyle(3.0f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
        vgc.PopState();
        vgc.StrokePath(path, GC( 0, 160, 192, 255 ), vg::StrokeStyle(3.0f, vg::VGLineCap::Round, vg::VGLineJoin::Round));
    }

    // Sample dots.
    for (i32 i = 0; i < 6; ++i)
    {
        const f32 sx = x + static_cast<f32>(i) * dx;
        const f32 sy = y + h * (1.0f - samples[i] * 0.8f);
        {
            vg::PathBuilder pb;
            vg::ShapeBuilder::BuildCircle(Vec2{ sx, sy + 2.0f }, 4.0f, pb);
            vg::VGRadialGradientFill fill(Vec2{ sx, sy + 2.0f }, 6.0f);
            fill.AddStop(0.0f, GC(0, 0, 0, 32));
            fill.AddStop(1.0f, GC(0, 0, 0, 0));
            vgc.FillPath(pb.ToPath(), fill);
        }
        vgc.FillCircle(Vec2{ sx, sy }, 4.0f, GC( 0, 160, 192, 255 ));
        vgc.FillCircle(Vec2{ sx, sy }, 2.0f, GC( 220, 240, 255, 255 ));
    }
}

void VGSandbox::DrawScissor(vg::VGContext& vgc, f32 x, f32 y, f32 t)
{
    vgc.PushState();
    vgc.Translate(x, y);
    vgc.Rotate(5.0f * kPi / 180.0f);
    vgc.FillRect(Rect{ -20, -20, 60, 40 }, GC( 255, 0, 0, 255 ));

    vgc.PushClipRect(Rect{ -20, -20, 60, 40 });
    vgc.Translate(40, 0);
    vgc.Rotate(Sin(t) * 0.15f);

    vgc.PopClip();
    vgc.FillRect(Rect{ -20, -10, 60, 30 }, GC( 255, 128, 0, 64 ));

    vgc.PushClipRect(Rect{ -60, -30, 60, 40 });
    vgc.FillRect(Rect{ -20, -10, 60, 30 }, GC( 255, 128, 0, 255 ));
    vgc.PopClip();

    vgc.PopState();
}

void VGSandbox::DrawImages(vg::VGContext& vgc, f32 x, f32 y, f32 t)
{
    const f32 tw = static_cast<f32>(m_checker.Width());
    const f32 th = static_cast<f32>(m_checker.Height());

    vgc.DrawImage(&m_checker, Vec2{ x, y });
    vgc.DrawImage(&m_checker, Rect{ x + 140, y, 80, 50 });
    vgc.DrawImage(&m_checker, Rect{ x + 230, y, 80, 80 }, Rect{ 0, 0, tw, th }, GC( 255, 120, 120, 220 ));
    vgc.DrawImage(&m_checker, Rect{ x + 320, y, 80, 80 }, Rect{ 0, 0, 64, 64 }, Color::White);

    vgc.PushState();
    vgc.Translate(x + 450, y + 40);
    vgc.Rotate(t * 0.6f);
    vgc.DrawImage(&m_checker, Rect{ -40, -40, 80, 80 });
    vgc.PopState();
}

void VGSandbox::DrawTextDemo(vg::VGContext& vgc, f32 x, f32 y, f32 t)
{
    if (!HasFonts()) return;

    vgc.DrawText(u8"Raptor.VG text rendering", m_fontLarge, Vec2{ x, y + 30 }, GC( 240, 240, 245, 255 ));
    vgc.DrawText(u8"Medium size - the quick brown fox", m_fontMedium, Vec2{ x, y + 60 }, GC( 180, 200, 255, 255 ));
    vgc.DrawText(u8"small caption @ 14px", m_fontSmall, Vec2{ x, y + 82 }, GC( 160, 170, 180, 255 ));

    const u8 pulse = static_cast<u8>(160.0f + Sin(t * 2.0f) * 60.0f);
    vgc.DrawText(u8"pulsing tint", m_fontMedium, Vec2{ x, y + 108 }, GC( 255, pulse, 80, 255 ));

    const Rect boxRect{ x + 350, y + 50, 200, 60 };
    vgc.StrokeRect(boxRect, GC( 80, 100, 120, 255 ), 1.0f);
    vgc.DrawText(u8"centered", m_fontMedium->font, m_fontMedium->atlas, m_fontService->GetAtlasTexture(m_fontMedium),
                 boxRect, fonts::TextAlignment::Center, fonts::VerticalAlignment::Middle, GC( 220, 220, 220, 255 ));

    vgc.PushState();
    vgc.Translate(x + 280, y + 130);
    vgc.Rotate(Sin(t * 0.7f) * 0.3f);
    vgc.DrawText(u8"rotated!", m_fontLarge, Vec2{ -60, 10 }, GC( 120, 255, 160, 255 ));
    vgc.PopState();
}

void VGSandbox::DrawUIConvenience(vg::VGContext& vgc, f32 x, f32 y)
{
    vgc.DrawLine(Vec2{ x, y + 5 }, Vec2{ x + 120, y + 5 }, GC( 200, 220, 255, 255 ), 1.0f);
    vgc.DrawLine(Vec2{ x, y + 15 }, Vec2{ x + 120, y + 15 }, GC( 200, 220, 255, 255 ), 2.5f);
    vgc.DrawLine(Vec2{ x, y + 30 }, Vec2{ x + 120, y + 30 }, GC( 200, 220, 255, 255 ), 5.0f);

    vgc.StrokeEllipse(Vec2{ x + 180, y + 20 }, 40, 18, GC( 255, 200, 140, 255 ), 2.0f);

    const Rect compareRect{ x + 250, y, 60, 40 };
    vgc.DrawBorderRect(compareRect, GC( 120, 255, 160, 255 ), 4.0f);
    vgc.StrokeRect(compareRect, GC( 255, 255, 255, 60 ), 1.0f);

    vgc.DrawBorderRoundedRect(Rect{ x + 330, y, 80, 40 }, 10.0f, GC( 180, 160, 255, 255 ), 3.0f);
    vgc.DrawBorderRoundedRect(Rect{ x + 430, y, 80, 40 }, vg::CornerRadii(2, 12, 2, 12), GC( 255, 180, 220, 255 ), 2.0f);
}

void VGSandbox::DrawImmediatePath(vg::VGContext& vgc, f32 x, f32 y, f32 t)
{
    // Animated "signature" curve.
    vgc.BeginPath();
    vgc.MoveTo(x, y + 25);
    vgc.CubicTo(x + 30, y + 25 - Sin(t * 1.5f) * 15, x + 60, y + 25 + Sin(t * 1.5f) * 15, x + 90, y + 25);
    vgc.CubicTo(x + 120, y + 25 - Sin(t * 1.5f + 1) * 15, x + 150, y + 25 + Sin(t * 1.5f + 1) * 15, x + 180, y + 25);
    vgc.Stroke(GC( 120, 200, 255, 255 ), 3.0f);

    // Filled star.
    const f32 cx = x + 230, cy = y + 25, outerR = 22.0f, innerR = 10.0f;
    const i32 pointCount = 5;
    vgc.BeginPath();
    for (i32 i = 0; i < pointCount * 2; ++i)
    {
        const f32 r = (i % 2 == 0) ? outerR : innerR;
        const f32 angle = static_cast<f32>(i) * kPi / static_cast<f32>(pointCount) - kPi * 0.5f;
        const f32 px = cx + Cos(angle) * r, py = cy + Sin(angle) * r;
        if (i == 0) vgc.MoveTo(px, py); else vgc.LineTo(px, py);
    }
    vgc.ClosePath();
    vgc.Fill(GC( 255, 220, 120, 255 ));

    // Teardrop with quads.
    vgc.BeginPath();
    vgc.MoveTo(x + 290, y + 5);
    vgc.QuadTo(x + 320, y + 25, x + 290, y + 45);
    vgc.QuadTo(x + 260, y + 25, x + 290, y + 5);
    vgc.ClosePath();
    vgc.Fill(GC( 220, 140, 255, 255 ));
    vgc.Stroke(GC( 255, 255, 255, 120 ), 1.0f);
}

void VGSandbox::DrawSVGDemo(vg::VGContext& vgc, f32 x, f32 y)
{
    if (HasFonts())
        vgc.DrawText(u8"SVG Rendering", m_fontMedium, Vec2{ x, y + 16 }, GC( 240, 240, 245, 255 ));

    if (m_hasBadge)
    {
        svg::SVGRenderer::Render(vgc, m_badge, Rect{ x, y + 24, 80, 80 });
        svg::SVGRenderer::Render(vgc, m_badge, Rect{ x + 90, y + 34, 48, 48 });
        svg::SVGRenderer::Render(vgc, m_badge, Rect{ x + 148, y + 34, 48, 48 }, Optional<Color>(GC( 255, 120, 80, 255 )));
    }
    if (m_hasIcon)
    {
        svg::SVGRenderer::Render(vgc, m_icon, Rect{ x + 210, y + 30, 56, 56 });
        svg::SVGRenderer::Render(vgc, m_icon, Rect{ x + 272, y + 38, 40, 40 });
        svg::SVGRenderer::Render(vgc, m_icon, Rect{ x + 318, y + 46, 28, 28 });
        svg::SVGRenderer::Render(vgc, m_icon, Rect{ x + 352, y + 50, 20, 20 });
    }
}

void VGSandbox::DrawDFTextDemo(vg::VGContext& vgc, f32 x, f32 y)
{
    if (!m_fontDF) return;

    // Label (using regular rasterized font).
    if (m_fontSmall)
        vgc.DrawText(u8"Distance Field Text (one atlas, multiple scales):", m_fontSmall, Vec2{ x, y + 12 }, GC( 180, 180, 190, 255 ));

    // Draw DF text at native size to verify baseline alignment and descenders.
    const f32 ascent = m_fontDF->font->Metrics().ascent;
    const f32 lineH = m_fontDF->font->Metrics().lineHeight;

    vgc.DrawText(u8"Typography", m_fontDF, Vec2{ x, y + 20 + ascent }, GC( 255, 220, 100, 255 ));

    // Scaled version to demonstrate resolution independence.
    vgc.PushState();
    vgc.Translate(x, y + 20 + ascent + lineH + 4.0f);
    vgc.Scale(0.6f, 0.6f);
    vgc.DrawText(u8"Typography", m_fontDF, Vec2{ 0, ascent }, GC( 200, 255, 200, 255 ));
    vgc.PopState();
}

Color VGSandbox::HSLToColor(f32 h, f32 s, f32 l)
{
    h = h - static_cast<f32>(static_cast<i32>(h)); // h % 1
    if (h < 0.0f) h += 1.0f;

    f32 r, g, b;
    if (s <= 0.0f) { r = g = b = l; }
    else
    {
        const f32 q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
        const f32 p = 2.0f * l - q;
        r = HueToRGB(p, q, h + 1.0f / 3.0f);
        g = HueToRGB(p, q, h);
        b = HueToRGB(p, q, h - 1.0f / 3.0f);
    }
    return GC( static_cast<u8>(r * 255.0f), static_cast<u8>(g * 255.0f), static_cast<u8>(b * 255.0f), 255 );
}

f32 VGSandbox::HueToRGB(f32 p, f32 q, f32 t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void VGSandbox::OnRender()
{
    if (m_fenceVal > 0) m_fence->Wait(m_fenceVal, ~0ull);
    if (m_swapChain->AcquireNextImage() != ErrorCode::Ok) return;

    m_vg->Clear();
    DrawScene(*m_vg, static_cast<f32>(m_width), static_cast<f32>(m_height), m_totalTime);
    vg::VGBatch& batch = m_vg->GetBatch();

    m_renderer.BeginFrame(static_cast<i32>(m_frameIndex));
    const vgr::VGRenderSlice slice = m_renderer.Prepare(batch, static_cast<i32>(m_frameIndex), m_width, m_height);

    m_pool->Reset();
    rhi::CommandEncoder* enc = nullptr;
    if (m_pool->CreateEncoder(enc) != ErrorCode::Ok || enc == nullptr) return;

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::Undefined, rhi::ResourceState::RenderTarget);

    rhi::ColorAttachment ca{};
    ca.view = m_swapChain->CurrentTextureView();
    ca.loadOp = rhi::LoadOp::Clear; ca.storeOp = rhi::StoreOp::Store;
    ca.clearValue = rhi::ClearColor(0.19f, 0.19f, 0.21f, 1.0f);
    rhi::RenderPassDesc rpd{};
    rpd.colorAttachments.Add(ca);

    rhi::RenderPassEncoder* rp = enc->BeginRenderPass(rpd);
    m_renderer.Render(*rp, m_width, m_height, static_cast<i32>(m_frameIndex), slice);
    rp->End();

    enc->TransitionTexture(m_swapChain->CurrentTexture(), rhi::ResourceState::RenderTarget, rhi::ResourceState::Present);

    rhi::CommandBuffer* cb = enc->Finish();
    ++m_fenceVal;
    rhi::CommandBuffer* cbs[1] = { cb };
    m_graphicsQueue->Submit(Span<rhi::CommandBuffer* const>(cbs, 1), m_fence, m_fenceVal);

    m_swapChain->Present(m_graphicsQueue);
    m_pool->DestroyEncoder(enc);

    m_frameIndex = (m_frameIndex + 1) % kFrames;
}

void VGSandbox::OnShutdown()
{
    if (m_device) m_device->WaitIdle();
    m_renderer.Dispose();
    m_vg.Reset();
    fonts::DFFonts::Shutdown();
    m_fontService.Reset();
    if (m_fence) m_device->DestroyFence(m_fence);
    if (m_pool)  m_device->DestroyCommandPool(m_pool);
    if (m_dfFs)  m_device->DestroyShaderModule(m_dfFs);
    if (m_fs)    m_device->DestroyShaderModule(m_fs);
    if (m_vs)    m_device->DestroyShaderModule(m_vs);
    if (m_compiler) { m_compiler->Destroy(); delete m_compiler; }
}

int main(int argc, char** argv) { VGSandbox app; return app.Run(argc, argv); }
