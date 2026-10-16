// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :viewport_capture partition.
//
// ViewportCaptureRecorder: a page's viewport capture, the three steps every capturing page takes:
// Request arms it, Record copies the composed colour target once the page's frame is fully drawn
// (from OnAfterSceneRender), and Complete writes the PNG on the next update, once the GPU has run
// the copy. The scene page and the Game page both capture through one of these; State says where
// the latest request stands (viewport_screenshot and pie_screenshot read it).
module;
#include "Core/Prelude.h"

export module editor.scene:viewport_capture;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.rhi.capture; // ScreenshotCapture
import :scene_page_interface; // ViewportCapture

using namespace foundation::core;

export namespace editor
{
    class ViewportCaptureRecorder
    {
    public:
        /// The latest request.
        [[nodiscard]] const ViewportCapture& State() const noexcept { return m_state; }
        /// A request waits for the next rendered frame.
        [[nodiscard]] bool Armed() const noexcept { return m_screenshot.Armed(); }

        /// Arms the next Record to write `path`, replacing a pending request.
        void Request(StringView path)
        {
            m_state = ViewportCapture{};
            m_state.state = ViewportCaptureState::Pending;
            m_state.path = String(path);
            m_screenshot.Request(path);
        }

        /// Records the copy of `target` (in `targetState`, where it is left) when a request is
        /// armed; a failure to record is the request's failure, logged by the capture. The PNG is
        /// what was drawn, never resampled; `renderWidth` x `renderHeight` names the resolution
        /// the content plays at when the view drew it scaled (reported beside the written size).
        void Record(foundation::rhi::Device* device, foundation::rhi::CommandEncoder* encoder,
                    foundation::rhi::Texture* target, foundation::rhi::TextureFormat format,
                    u32 width, u32 height, foundation::rhi::ResourceState targetState, u32 originX = 0,
                    u32 originY = 0, u32 renderWidth = 0, u32 renderHeight = 0)
        {
            if (!m_screenshot.Armed() || device == nullptr || encoder == nullptr)
            {
                return;
            }
            const bool scaled = renderWidth > 0 && renderHeight > 0 &&
                                (renderWidth != width || renderHeight != height);
            m_state.renderWidth = scaled ? renderWidth : 0;
            m_state.renderHeight = scaled ? renderHeight : 0;
            if (!m_screenshot.Record(*device, *encoder, target, format, width, height, targetState, originX,
                                     originY))
            {
                m_state.state = ViewportCaptureState::Failed; // logged by the capture
            }
        }

        /// Writes the PNG of a copy recorded last frame. A one-off, so it waits for the whole
        /// GPU, then maps and writes.
        void Complete(foundation::rhi::Device* device, IAllocator& allocator)
        {
            if (!m_screenshot.Recorded() || device == nullptr)
            {
                return;
            }
            device->WaitIdle();
            foundation::image::Image written;
            const Status saved = m_screenshot.Complete(*device, allocator, written);
            m_state.state = saved.IsOk() ? ViewportCaptureState::Written : ViewportCaptureState::Failed;
            // (renderWidth / renderHeight stay as Record set them)
            m_state.width = written.Width();
            m_state.height = written.Height();
        }

        /// Drops the readback buffer; the page calls it on close, while the device lives.
        void Release(foundation::rhi::Device* device)
        {
            if (device != nullptr)
            {
                m_screenshot.Release(*device);
            }
        }

    private:
        foundation::rhi::ScreenshotCapture m_screenshot;
        ViewportCapture m_state;
    };
}
