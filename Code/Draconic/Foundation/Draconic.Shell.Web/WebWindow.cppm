// Draconic::ShellWeb - `draconic.shell.web:window`.
//
// The web shell's window IS an HTML <canvas>. Its size comes from the live canvas
// (emscripten_get_canvas_element_size), and Native() hands back the canvas CSS selector, which the
// WebGPU RHI turns into a surface (WGPUEmscriptenSurfaceSourceCanvasHTMLSelector - see the WebGPU
// backend's CreateSurface). CSS can resize the canvas at any time, so QuerySize() re-polls it.

module;
#include "Draconic.Foundation/Prelude.h"
#include <emscripten/html5.h>

export module draconic.shell.web:window;

import draconic.foundation;
import draconic.shell;

namespace foundation = draconic::foundation;

export namespace draconic::shell
{
    class WebWindow final : public IWindow
    {
    public:
        WebWindow(foundation::u32 id, foundation::StringView selector, const WindowSettings& settings)
            : m_id(id), m_selector(selector), m_width(settings.width), m_height(settings.height)
        {
            QuerySize(); // seed from the live canvas if it is already sized
        }

        [[nodiscard]] foundation::u32 Id() const noexcept override { return m_id; }
        [[nodiscard]] foundation::u32 Width() const noexcept override { return m_width; }
        [[nodiscard]] foundation::u32 Height() const noexcept override { return m_height; }
        [[nodiscard]] foundation::i32 X() const noexcept override { return 0; } // canvas has no screen pos
        [[nodiscard]] foundation::i32 Y() const noexcept override { return 0; }
        void SetPosition(foundation::i32, foundation::i32) override {} // meaningless for a canvas
        void SetSize(foundation::u32 width, foundation::u32 height) override
        {
            m_width = width;
            m_height = height;
            emscripten_set_canvas_element_size(reinterpret_cast<const char*>(m_selector.CStr()),
                                               static_cast<int>(width), static_cast<int>(height));
        }
        [[nodiscard]] foundation::f32 ContentScale() const noexcept override
        {
            const double ratio = emscripten_get_device_pixel_ratio();
            return ratio > 0.0 ? static_cast<foundation::f32>(ratio) : 1.0f;
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

        void StartTextInput() override { m_textInputActive = true; }
        void StopTextInput() override { m_textInputActive = false; }
        [[nodiscard]] bool IsTextInputActive() const noexcept override { return m_textInputActive; }

        [[nodiscard]] foundation::StringView Selector() const noexcept { return m_selector.AsView(); }

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
            const foundation::u32 nw = static_cast<foundation::u32>(w);
            const foundation::u32 nh = static_cast<foundation::u32>(h);
            if (nw == m_width && nh == m_height)
            {
                return false;
            }
            m_width = nw;
            m_height = nh;
            return true;
        }

    private:
        foundation::u32 m_id;
        foundation::String m_selector; // canvas CSS selector, e.g. "#canvas" (kept alive for Native())
        foundation::u32 m_width;
        foundation::u32 m_height;
        bool m_open = true;
        bool m_textInputActive = false;
    };
}
