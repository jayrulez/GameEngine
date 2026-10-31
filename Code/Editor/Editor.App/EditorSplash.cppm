// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :splash partition.
//
// EditorSplash: what the editor shows from launch until its main window is ready. Startup runs on
// the main thread with nothing drawn (the graphics device, the shader compiles, the fonts, the
// project and its pages), and the editor's own UI cannot draw until its shaders exist, so the
// splash is drawn on the CPU into a software window (shell WindowSettings::softwareSurface,
// IWindow::PresentPixels), opened before there is a graphics device: the AssiduousEngine logo, a
// line saying what startup is doing, a progress bar and the version. Each Step redraws it and
// pumps the window events so the window stays responsive. It is closed when the main window shows.
// It draws at the display's scale (a 175% display gets a splash 1.75 times the size, its text
// rasterized at that size and its logo from a picture big enough), so it is sharp and not small.
//
// The drawing is a plain RGBA buffer (Pixels), so a test checks it without a window.

module;
#include "Core/Prelude.h"

export module editor.app:splash;

import foundation.core;
import foundation.shell;
import foundation.image;
import foundation.fonts.truetype;

using namespace foundation::core;

export namespace editor::app
{
    namespace shell = foundation::shell;

    /// EditorSplash::kHeight, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kEditorSplashHeight = 300;

    /// EditorSplash::kWidth, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr u32 kEditorSplashWidth = 560;

    class EditorSplash
    {
    public:
        static constexpr u32 kWidth = kEditorSplashWidth;
        static constexpr u32 kHeight = kEditorSplashHeight;

        /// `scale` is the display's (1 at 100%): the splash is kWidth x kHeight times it.
        /// `logoRgba` is the logo at the size to draw (LoadEditorSplashLogo at `scale`, tightly
        /// packed RGBA), `fontData` a TrueType face for the text; either may be empty (the splash
        /// draws without it). `window` may be null (a test draws without showing).
        EditorSplash(shell::IShell* shellRef, shell::IWindow* window, Span<const u8> logoRgba, u32 logoWidth,
                     u32 logoHeight, Array<u8> fontData, StringView version, f32 scale);

        /// Says what startup is doing, with `progress` in 0..1, and shows it.
        void Step(StringView status, f32 progress);

        /// Closes the window once the main window is up; the shell destroys it at its next flush.
        void Close();

        /// The drawn image, Width() x Height() RGBA.
        [[nodiscard]] Span<const u8> Pixels() const noexcept { return {m_pixels.Data(), m_pixels.Size()}; }
        /// The drawn image's size: kWidth x kHeight at the scale, in pixels.
        [[nodiscard]] u32 Width() const noexcept { return m_width; }
        [[nodiscard]] u32 Height() const noexcept { return m_height; }
        [[nodiscard]] f32 Scale() const noexcept { return m_scale; }
        [[nodiscard]] StringView Status() const noexcept { return m_status.AsView(); }
        [[nodiscard]] f32 Progress() const noexcept { return m_progress; }

        /// Draws the current state into Pixels (Step does this, then shows it).
        void Render();

    private:
        void Fill(i32 x, i32 y, i32 w, i32 h, Color color);
        void Blend(i32 x, i32 y, Color color, f32 alpha);
        void DrawText(StringView text, i32 x, i32 baseline, Color color, bool alignRight = false);
        [[nodiscard]] i32 MeasureText(StringView text) const;
        /// A length of the 100% layout at the splash's scale, in pixels.
        [[nodiscard]] i32 Scaled(i32 length) const noexcept;

        shell::IShell* m_shell = nullptr;
        shell::IWindow* m_window = nullptr;
        Array<u8> m_logo;
        u32 m_logoWidth = 0;
        u32 m_logoHeight = 0;
        foundation::fonts::TrueTypeFont m_font;
        bool m_hasFont = false;
        String m_version;
        String m_status;
        f32 m_progress = 0.0f;
        f32 m_scale = 1.0f;
        u32 m_width = kEditorSplashWidth;
        u32 m_height = kEditorSplashHeight;
        Array<u8> m_pixels;
    };

    /// Opens the splash over a new software window of `shell` (after the main window: the shell's
    /// first window is its main one), with the logo from <dataRoot>/Branding and the text
    /// in the font at `fontPath`, else `embeddedFont` (the face built into the editor). Null when
    /// the window cannot open: the editor then starts without one.
    [[nodiscard]] UniquePtr<EditorSplash> OpenEditorSplash(shell::IShell& shellRef, StringView dataRoot,
                                                           StringView fontPath, Span<const u8> embeddedFont,
                                                           StringView version, IAllocator& allocator);

    /// The splash's logo at `scale` (1 at 100%), from <dataRoot>/Branding: the 1x picture
    /// (assiduous-logo-splash.png, 440 x 95) at 100%, else the 2x one (assiduous-logo-splash@2x.png,
    /// rendered from the SVG by scripts/logo-splash.py) resampled to the size the scale asks for.
    /// False when the picture does not load.
    [[nodiscard]] bool LoadEditorSplashLogo(StringView dataRoot, f32 scale, foundation::image::Image& out);

    /// The AssiduousEngine logo (mark and wordmark, for a dark background), relative to the data
    /// root: what a page's markup shows (`<Image src="...">`, through the editor's resources). Its
    /// aspect is kEditorLogoAspect (width over height).
    inline constexpr const char8_t* kEditorLogoPath = u8"Branding/assiduous-logo.svg";
    inline constexpr f32 kEditorLogoAspect = 931.0f / 200.0f;

    /// The AssiduousEngine icon on `window` (its title bar, the taskbar), from
    /// <dataRoot>/Branding/assiduous-icon-<size>.png: 32 px at the normal scale, the larger
    /// sizes for scaled displays. False when none of them loads.
    bool ApplyEditorWindowIcon(shell::IWindow& window, StringView dataRoot);
}
