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
// pumps the window events so the window stays responsive. Close when the main window is shown.
//
// The drawing is a plain RGBA buffer (Pixels), so a test checks it without a window.

module;
#include "Core/Prelude.h"

export module editor.app:splash;

import foundation.core;
import foundation.shell;
import foundation.image;
import foundation.fonts.truetype;
import foundation.ui;

using namespace foundation::core;

export namespace editor::app
{
    namespace shell = foundation::shell;

    class EditorSplash
    {
    public:
        static constexpr u32 kWidth = 560;
        static constexpr u32 kHeight = 300;

        /// `logoRgba` is the logo at the splash's size (Assets/branding/assiduous-logo-splash.png,
        /// tightly packed RGBA), `fontData` a TrueType face for the text; either may be empty (the
        /// splash draws without it). `window` may be null (a test draws without showing).
        EditorSplash(shell::IShell* shellRef, shell::IWindow* window, Span<const u8> logoRgba, u32 logoWidth,
                     u32 logoHeight, Array<u8> fontData, StringView version);

        /// Say what startup is doing, `progress` in 0..1, and show it.
        void Step(StringView status, f32 progress);

        /// Close the window (the main window is up): the shell destroys it at its next flush.
        void Close();

        /// The drawn image, kWidth x kHeight RGBA.
        [[nodiscard]] Span<const u8> Pixels() const noexcept { return {m_pixels.Data(), m_pixels.Size()}; }
        [[nodiscard]] StringView Status() const noexcept { return m_status.AsView(); }
        [[nodiscard]] f32 Progress() const noexcept { return m_progress; }

        /// Draw the current state into Pixels (Step does, then shows it).
        void Render();

    private:
        void Fill(i32 x, i32 y, i32 w, i32 h, Color color);
        void Blend(i32 x, i32 y, Color color, f32 alpha);
        void DrawText(StringView text, i32 x, i32 baseline, Color color, bool alignRight = false);
        [[nodiscard]] i32 MeasureText(StringView text) const;

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
        Array<u8> m_pixels;
    };

    /// Open the splash over a new software window of `shell` (after the main window: the shell's
    /// first window is its main one), with the logo from <dataRoot>/Assets/branding and the text
    /// in the font at `fontPath`, else `embeddedFont` (the face built into the editor). Null when
    /// the window cannot open: the editor then starts without one.
    [[nodiscard]] UniquePtr<EditorSplash> OpenEditorSplash(shell::IShell& shellRef, StringView dataRoot,
                                                           StringView fontPath, Span<const u8> embeddedFont,
                                                           StringView version, IAllocator& allocator);

    /// The AssiduousEngine logo (mark and wordmark, for a dark background) as a drawable, from
    /// <dataRoot>/Assets/branding/assiduous-logo.svg; null when it does not load. Its aspect is
    /// kEditorLogoAspect (width over height).
    [[nodiscard]] foundation::ui::DrawablePtr LoadEditorLogo(StringView dataRoot, IAllocator& allocator);
    inline constexpr f32 kEditorLogoAspect = 931.0f / 200.0f;

    /// The AssiduousEngine icon on `window` (its title bar, the taskbar), from
    /// <dataRoot>/Assets/branding/assiduous-icon-<size>.png: 32 px at the normal scale, the larger
    /// sizes for scaled displays. False when none of them loads.
    bool ApplyEditorWindowIcon(shell::IWindow& window, StringView dataRoot);
}
