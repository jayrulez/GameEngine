// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Shell.Web - `foundation.shell.web:window`.
//
// The web shell's window IS an HTML <canvas>. Its size comes from the live canvas
// (emscripten_get_canvas_element_size), and Native() hands back the canvas CSS selector, which the
// WebGPU RHI turns into a surface (WGPUEmscriptenSurfaceSourceCanvasHTMLSelector - see the WebGPU
// backend's CreateSurface). CSS can resize the canvas at any time, so QuerySize() re-polls it.

module;
#include "Core/Prelude.h"
#include <emscripten/html5.h>

export module foundation.shell.web:window;

import foundation.core;
import foundation.shell;

namespace core = foundation::core;

export namespace foundation::shell
{
    class WebWindow final : public IWindow
    {
    public:
        WebWindow(core::u32 id, core::StringView selector, const WindowSettings& settings)
            : m_id(id), m_selector(selector), m_width(settings.width), m_height(settings.height)
        {
            QuerySize(); // seed from the live canvas if it is already sized
        }

        [[nodiscard]] core::u32 Id() const noexcept override { return m_id; }
        [[nodiscard]] core::u32 Width() const noexcept override { return m_width; }
        [[nodiscard]] core::u32 Height() const noexcept override { return m_height; }
        [[nodiscard]] core::u32 PixelWidth() const noexcept override { return m_width; } // the canvas's own size is its drawing buffer's
        [[nodiscard]] core::u32 PixelHeight() const noexcept override { return m_height; }
        [[nodiscard]] core::i32 X() const noexcept override { return 0; } // canvas has no screen pos
        [[nodiscard]] core::i32 Y() const noexcept override { return 0; }
        void SetPosition(core::i32, core::i32) override {} // meaningless for a canvas
        void SetSize(core::u32 width, core::u32 height) override
        {
            m_width = width;
            m_height = height;
            emscripten_set_canvas_element_size(reinterpret_cast<const char*>(m_selector.CStr()),
                                               static_cast<int>(width), static_cast<int>(height));
        }
        [[nodiscard]] core::f32 ContentScale() const noexcept override
        {
            const double ratio = emscripten_get_device_pixel_ratio();
            return ratio > 0.0 ? static_cast<core::f32>(ratio) : 1.0f;
        }
        [[nodiscard]] NativeWindow Native() const noexcept override
        {
            // The RHI reads `window` as the canvas CSS selector (const char*); see NativeWindow.
            NativeWindow native;
            native.system = WindowSystem::Web;
            native.display = nullptr;
            native.window = const_cast<void*>(static_cast<const void*>(m_selector.CStr()));
            return native;
        }
        [[nodiscard]] bool IsOpen() const noexcept override { return m_open; }
        [[nodiscard]] bool IsMinimized() const noexcept override { return false; }
        void Close() override { m_open = false; }
        void Raise() override {} // the page's one canvas: nothing to bring forward
        void SetTitle(core::StringView) override {} // the page's title is the page's, not the canvas's

        void StartTextInput(TextInputKind kind) override
        {
            m_textInputActive = true;
            m_textInputKind = kind;
        }
        void StopTextInput() override { m_textInputActive = false; }
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return m_textInputActive; }
        [[nodiscard]] TextInputKind GetTextInputKind() const noexcept override { return m_textInputKind; }

        [[nodiscard]] core::StringView Selector() const noexcept { return m_selector.AsView(); }

        // Poll the live canvas size; returns true and updates Width/Height when it changed (so the
        // manager can emit a Resized event).
        bool QuerySize() noexcept
        {
            int w = 0;
            int h = 0;
            if (emscripten_get_canvas_element_size(reinterpret_cast<const char*>(m_selector.CStr()),
                                                   &w, &h) != EMSCRIPTEN_RESULT_SUCCESS)
            {
                return false;
            }
            if (w <= 0 || h <= 0)
            {
                return false;
            }
            const core::u32 nw = static_cast<core::u32>(w);
            const core::u32 nh = static_cast<core::u32>(h);
            if (nw == m_width && nh == m_height)
            {
                return false;
            }
            m_width = nw;
            m_height = nh;
            return true;
        }

    private:
        core::u32 m_id;
        core::String m_selector; // canvas CSS selector, e.g. "#canvas" (kept alive for Native())
        core::u32 m_width;
        core::u32 m_height;
        bool m_open = true;
        bool m_textInputActive = false;
        TextInputKind m_textInputKind = TextInputKind::Text;
    };
}
