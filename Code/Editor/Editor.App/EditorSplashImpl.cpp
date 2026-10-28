// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :splash partition (implementation). See EditorSplash.cppm.

module;
#include "Core/Prelude.h"

module editor.app;

import foundation.core;
import foundation.shell;
import foundation.fonts;
import foundation.fonts.truetype;
import foundation.image;
import foundation.image.io;

using namespace foundation::core;

namespace editor::app
{
    namespace
    {
        constexpr Color kBackground{0x1F / 255.0f, 0x22 / 255.0f, 0x27 / 255.0f, 1.0f};
        constexpr Color kBorder{0x3A / 255.0f, 0x3D / 255.0f, 0x42 / 255.0f, 1.0f};
        constexpr Color kTrack{0x2E / 255.0f, 0x32 / 255.0f, 0x38 / 255.0f, 1.0f};
        constexpr Color kAccent{0xF5 / 255.0f, 0x8F / 255.0f, 0x42 / 255.0f, 1.0f};
        constexpr Color kText{0.78f, 0.79f, 0.81f, 1.0f};
        constexpr Color kDim{0.50f, 0.51f, 0.53f, 1.0f};
        // The layout at 100%; Scaled takes each to the splash's scale.
        constexpr i32 kMargin = 40;
        constexpr i32 kLogoY = 84;
        constexpr i32 kStatusBaseline = 236;
        constexpr i32 kBarY = 254;
        constexpr i32 kBarHeight = 4;
        constexpr i32 kVersionFromBottom = 18;
        constexpr f32 kFontSize = 15.0f;
        constexpr u32 kLogoWidth = 440; // the 1x picture's width
        constexpr u32 kLogoHeight = 95;

        // `src` (sw x sh straight RGBA) resampled to dw x dh: bilinear, in premultiplied alpha so
        // the logo's edges do not take the colour of its transparent pixels. From the 2x picture
        // to anything between 1x and 2x, so no step skips a source pixel.
        [[nodiscard]] foundation::image::Image Resampled(Span<const u8> src, u32 sw, u32 sh, u32 dw, u32 dh)
        {
            Array<u8> out;
            out.Resize(static_cast<usize>(dw) * dh * 4);
            const auto texel = [&](i32 x, i32 y, i32 c) -> f32
            {
                x = Clamp(x, 0, static_cast<i32>(sw) - 1);
                y = Clamp(y, 0, static_cast<i32>(sh) - 1);
                const u8* p = &src[(static_cast<usize>(y) * sw + static_cast<usize>(x)) * 4];
                const f32 alpha = p[3] / 255.0f;
                return c == 3 ? alpha : (p[c] / 255.0f) * alpha;
            };
            for (u32 y = 0; y < dh; ++y)
            {
                const f32 fy = (static_cast<f32>(y) + 0.5f) * static_cast<f32>(sh) / static_cast<f32>(dh) - 0.5f;
                const i32 y0 = static_cast<i32>(Floor(fy));
                const f32 ty = fy - static_cast<f32>(y0);
                for (u32 x = 0; x < dw; ++x)
                {
                    const f32 fx = (static_cast<f32>(x) + 0.5f) * static_cast<f32>(sw) / static_cast<f32>(dw) - 0.5f;
                    const i32 x0 = static_cast<i32>(Floor(fx));
                    const f32 tx = fx - static_cast<f32>(x0);
                    f32 value[4];
                    for (i32 c = 0; c < 4; ++c)
                    {
                        const f32 top = texel(x0, y0, c) * (1.0f - tx) + texel(x0 + 1, y0, c) * tx;
                        const f32 bottom = texel(x0, y0 + 1, c) * (1.0f - tx) + texel(x0 + 1, y0 + 1, c) * tx;
                        value[c] = top * (1.0f - ty) + bottom * ty;
                    }
                    u8* q = &out[(static_cast<usize>(y) * dw + x) * 4];
                    const f32 alpha = value[3];
                    for (i32 c = 0; c < 3; ++c)
                        q[c] = static_cast<u8>(Clamp(alpha > 0.0f ? value[c] / alpha : 0.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
                    q[3] = static_cast<u8>(Clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
            return foundation::image::Image(dw, dh, foundation::image::PixelFormat::RGBA8, Span<const u8>(out.Data(), out.Size()));
        }

        [[nodiscard]] u32 DecodeOne(StringView text, usize& i)
        {
            const u8 c = static_cast<u8>(text[i]);
            u32 code = c;
            usize extra = 0;
            if (c >= 0xF0)
            {
                code = c & 0x07u;
                extra = 3;
            }
            else if (c >= 0xE0)
            {
                code = c & 0x0Fu;
                extra = 2;
            }
            else if (c >= 0xC0)
            {
                code = c & 0x1Fu;
                extra = 1;
            }
            ++i;
            for (usize k = 0; k < extra && i < text.Size(); ++k, ++i)
            {
                code = (code << 6) | (static_cast<u8>(text[i]) & 0x3Fu);
            }
            return code;
        }
    }

    EditorSplash::EditorSplash(shell::IShell* shellRef, shell::IWindow* window, Span<const u8> logoRgba, u32 logoWidth,
                               u32 logoHeight, Array<u8> fontData, StringView version, f32 scale)
        : m_shell(shellRef), m_window(window), m_version(version), m_scale(Max(scale, 1.0f))
    {
        m_width = static_cast<u32>(static_cast<f32>(kWidth) * m_scale + 0.5f);
        m_height = static_cast<u32>(static_cast<f32>(kHeight) * m_scale + 0.5f);
        if (logoRgba.Size() >= static_cast<usize>(logoWidth) * logoHeight * 4 && logoWidth > 0 && logoHeight > 0)
        {
            m_logo.Resize(static_cast<usize>(logoWidth) * logoHeight * 4);
            for (usize i = 0; i < m_logo.Size(); ++i)
            {
                m_logo[i] = logoRgba[i];
            }
            m_logoWidth = logoWidth;
            m_logoHeight = logoHeight;
        }
        if (!fontData.IsEmpty())
        {
            m_hasFont = m_font.Initialize(Move(fontData), kFontSize * m_scale) == foundation::fonts::FontLoadResult::Success;
        }
        m_pixels.Resize(static_cast<usize>(m_width) * m_height * 4);
        Render();
    }

    void EditorSplash::Step(StringView status, f32 progress)
    {
        m_status = String(status);
        m_progress = Clamp(progress, 0.0f, 1.0f);
        Render();
        if (m_window != nullptr)
        {
            (void)m_window->PresentPixels(Pixels(), m_width, m_height);
        }
        // Startup holds the main thread: pump the window events here so the desktop does not
        // take the editor for hung while it starts.
        if (m_shell != nullptr)
        {
            m_shell->ProcessEvents();
        }
    }

    void EditorSplash::Close()
    {
        if (m_window == nullptr)
        {
            return;
        }
        if (m_shell != nullptr && m_shell->WindowManager() != nullptr)
        {
            m_shell->WindowManager()->DestroyWindow(m_window);
        }
        else
        {
            m_window->Close();
        }
        m_window = nullptr;
    }

    UniquePtr<EditorSplash> OpenEditorSplash(shell::IShell& shellRef, StringView dataRoot, StringView fontPath,
                                             Span<const u8> embeddedFont, StringView version, IAllocator& allocator)
    {
        shell::IWindowManager* windows = shellRef.WindowManager();
        if (windows == nullptr)
        {
            return {};
        }
        shell::WindowSettings settings;
        settings.title = u8"AssiduousEngine";
        settings.width = EditorSplash::kWidth;
        settings.height = EditorSplash::kHeight;
        settings.resizable = false;
        settings.borderless = true;
        settings.softwareSurface = true;
        Result<shell::IWindow*> opened = windows->CreateWindow(settings);
        if (!opened.HasValue() || opened.Value() == nullptr)
        {
            return {};
        }
        shell::IWindow* window = opened.Value();
        (void)ApplyEditorWindowIcon(*window, dataRoot);

        // The display's scale (1.75 at 175%): the splash draws at it, and the window takes the size
        // that shows it pixel for pixel. A window already counted in units larger than its pixels
        // (macOS, Wayland) grows by the rest of the scale only; one that cannot say draws at 100%.
        const f32 scale = Max(window->ContentScale(), 1.0f);
        if (scale > 1.0f && window->Width() > 0)
        {
            const f32 density = static_cast<f32>(window->PixelWidth()) / static_cast<f32>(window->Width());
            const f32 units = scale / Max(density, 1.0f);
            if (units > 1.0f)
            {
                const u32 width = static_cast<u32>(static_cast<f32>(EditorSplash::kWidth) * units + 0.5f);
                const u32 height = static_cast<u32>(static_cast<f32>(EditorSplash::kHeight) * units + 0.5f);
                // Grown about its middle, so it stays where the shell centred it.
                window->SetPosition(window->X() - static_cast<i32>(width - window->Width()) / 2,
                                    window->Y() - static_cast<i32>(height - window->Height()) / 2);
                window->SetSize(width, height);
            }
        }

        foundation::image::Image logo;
        const bool hasLogo = LoadEditorSplashLogo(dataRoot, scale, logo);
        Array<u8> font;
        if (Result<Array<byte>> read = ReadFile(fontPath); read.HasValue())
        {
            font.Resize(read.Value().Size());
            for (usize i = 0; i < font.Size(); ++i)
            {
                font[i] = static_cast<u8>(read.Value()[i]);
            }
        }
        else
        {
            for (const u8 b : embeddedFont)
            {
                font.PushBack(b);
            }
        }
        return MakeUnique<EditorSplash>(allocator, &shellRef, window,
                                        hasLogo ? logo.PixelData() : Span<const u8>{}, hasLogo ? logo.Width() : 0u,
                                        hasLogo ? logo.Height() : 0u, Move(font), version, scale);
    }

    bool LoadEditorSplashLogo(StringView dataRoot, f32 scale, foundation::image::Image& out)
    {
        scale = Max(scale, 1.0f);
        const bool large = scale > 1.0f;
        foundation::image::Image picture;
        const String path = PathJoin(dataRoot, large ? u8"Branding/assiduous-logo-splash@2x.png" : u8"Branding/assiduous-logo-splash.png");
        if (!foundation::image::io::LoadImage(path.AsView(), picture).IsOk() || picture.Format() != foundation::image::PixelFormat::RGBA8)
            return false;
        const u32 width = static_cast<u32>(static_cast<f32>(kLogoWidth) * scale + 0.5f);
        const u32 height = static_cast<u32>(static_cast<f32>(kLogoHeight) * scale + 0.5f);
        out = (picture.Width() == width && picture.Height() == height)
                  ? Move(picture)
                  : Resampled(picture.PixelData(), picture.Width(), picture.Height(), width, height);
        return true;
    }

    i32 EditorSplash::Scaled(i32 length) const noexcept
    {
        return static_cast<i32>(static_cast<f32>(length) * m_scale + 0.5f);
    }

    bool ApplyEditorWindowIcon(shell::IWindow& window, StringView dataRoot)
    {
        static constexpr u32 kSizes[] = {32, 64, 128, 256}; // the first at the normal scale
        Array<foundation::image::Image> images;
        Array<shell::WindowIconImage> icon;
        for (const u32 size : kSizes)
        {
            const String path = PathJoin(dataRoot, Format(u8"Branding/assiduous-icon-{}.png", size).AsView());
            foundation::image::Image image;
            if (foundation::image::io::LoadImage(path.AsView(), image).IsOk() &&
                image.Format() == foundation::image::PixelFormat::RGBA8 && image.Width() == size)
            {
                images.PushBack(Move(image));
            }
        }
        for (const foundation::image::Image& image : images)
        {
            icon.PushBack(shell::WindowIconImage{image.PixelData().Data(), image.Width(), image.Height()});
        }
        if (icon.IsEmpty())
        {
            return false;
        }
        window.SetIcon(Span<const shell::WindowIconImage>(icon.Data(), icon.Size()));
        return true;
    }

    void EditorSplash::Render()
    {
        const i32 w = static_cast<i32>(m_width);
        const i32 h = static_cast<i32>(m_height);
        const i32 border = Max(1, Scaled(1));
        Fill(0, 0, w, h, kBorder);
        Fill(border, border, w - 2 * border, h - 2 * border, kBackground);

        // The logo, centred in the upper part.
        if (!m_logo.IsEmpty())
        {
            const i32 x0 = (w - static_cast<i32>(m_logoWidth)) / 2;
            const i32 y0 = Scaled(kLogoY);
            for (u32 y = 0; y < m_logoHeight; ++y)
            {
                for (u32 x = 0; x < m_logoWidth; ++x)
                {
                    const u8* p = &m_logo[(static_cast<usize>(y) * m_logoWidth + x) * 4];
                    if (p[3] != 0)
                    {
                        Blend(x0 + static_cast<i32>(x), y0 + static_cast<i32>(y),
                              Color{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1.0f}, p[3] / 255.0f);
                    }
                }
            }
        }

        // What startup is doing, the bar under it, and the version.
        const i32 margin = Scaled(kMargin);
        const i32 barY = Scaled(kBarY);
        const i32 barHeight = Max(1, Scaled(kBarHeight));
        DrawText(m_status.AsView(), margin, Scaled(kStatusBaseline), kText);
        const i32 barWidth = w - 2 * margin;
        Fill(margin, barY, barWidth, barHeight, kTrack);
        Fill(margin, barY, static_cast<i32>(static_cast<f32>(barWidth) * m_progress + 0.5f), barHeight, kAccent);
        DrawText(m_version.AsView(), w - margin, h - Scaled(kVersionFromBottom), kDim, /*alignRight*/ true);
    }

    void EditorSplash::Fill(i32 x, i32 y, i32 w, i32 h, Color color)
    {
        for (i32 row = Max(0, y); row < Min(static_cast<i32>(m_height), y + h); ++row)
        {
            for (i32 col = Max(0, x); col < Min(static_cast<i32>(m_width), x + w); ++col)
            {
                u8* p = &m_pixels[(static_cast<usize>(row) * m_width + static_cast<usize>(col)) * 4];
                p[0] = static_cast<u8>(color.r * 255.0f + 0.5f);
                p[1] = static_cast<u8>(color.g * 255.0f + 0.5f);
                p[2] = static_cast<u8>(color.b * 255.0f + 0.5f);
                p[3] = 255;
            }
        }
    }

    void EditorSplash::Blend(i32 x, i32 y, Color color, f32 alpha)
    {
        if (x < 0 || y < 0 || x >= static_cast<i32>(m_width) || y >= static_cast<i32>(m_height))
        {
            return;
        }
        u8* p = &m_pixels[(static_cast<usize>(y) * m_width + static_cast<usize>(x)) * 4];
        const auto mix = [alpha](u8 under, f32 over)
        { return static_cast<u8>(static_cast<f32>(under) * (1.0f - alpha) + over * 255.0f * alpha + 0.5f); };
        p[0] = mix(p[0], color.r);
        p[1] = mix(p[1], color.g);
        p[2] = mix(p[2], color.b);
        p[3] = 255;
    }

    i32 EditorSplash::MeasureText(StringView text) const
    {
        return m_hasFont ? static_cast<i32>(m_font.MeasureString(text) + 0.5f) : 0;
    }

    void EditorSplash::DrawText(StringView text, i32 x, i32 baseline, Color color, bool alignRight)
    {
        if (!m_hasFont || text.IsEmpty())
        {
            return;
        }
        f32 pen = static_cast<f32>(alignRight ? x - MeasureText(text) : x);
        Array<u8> coverage;
        usize i = 0;
        i32 previous = 0;
        while (i < text.Size())
        {
            const i32 code = static_cast<i32>(DecodeOne(text, i));
            if (previous != 0)
            {
                pen += m_font.GetKerning(previous, code);
            }
            i32 gw = 0, gh = 0, gx = 0, gy = 0;
            if (m_font.RasterizeGlyph(code, coverage, gw, gh, gx, gy))
            {
                const i32 left = static_cast<i32>(pen + 0.5f) + gx;
                for (i32 row = 0; row < gh; ++row)
                {
                    for (i32 col = 0; col < gw; ++col)
                    {
                        const u8 a = coverage[static_cast<usize>(row) * static_cast<usize>(gw) + static_cast<usize>(col)];
                        if (a != 0)
                        {
                            Blend(left + col, baseline + gy + row, color, a / 255.0f);
                        }
                    }
                }
            }
            pen += m_font.GetGlyphInfo(code).advanceWidth;
            previous = code;
        }
    }
}
