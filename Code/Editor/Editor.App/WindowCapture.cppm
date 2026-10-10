// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor App - :window_capture partition
//
// EditorWindowCapture: screenshots of the editor's own windows, UI and all, while it runs - the
// main window, or each floating one too. A request arms one capture per window; that window's
// next frame records a copy of its finished backbuffer (after its UI has drawn); the next update
// writes the PNG once the GPU has run the copy. editor_screenshot, the View > Screenshot action
// and the --screenshot launch flag all capture through this.
module;
#include "Core/Prelude.h"

export module editor.app:window_capture;

import foundation.core;
import foundation.rhi;
import foundation.image;
import engine.defaultapp; // ScreenshotCapture

using namespace foundation::core;

export namespace editor::app
{
    enum class WindowCaptureState : u8
    {
        Pending, ///< requested; the window has not drawn and written it yet
        Written, ///< the PNG is at `path`, `width` x `height`
        Failed,  ///< the copy or the write failed, or the window closed first (the log says why)
    };

    /// One window to capture: the shell window's id, whether it is the main window, the PNG.
    struct WindowShotRequest
    {
        u32 window = 0;
        bool main = false;
        String path;
    };

    /// Where one requested capture stands.
    struct WindowShot
    {
        u32 window = 0;
        bool main = false;
        String path;
        WindowCaptureState state = WindowCaptureState::Pending;
        u32 width = 0;
        u32 height = 0;
    };

    class EditorWindowCapture
    {
    public:
        explicit EditorWindowCapture(IAllocator& allocator) : m_allocator(&allocator), m_slots(allocator) {}

        /// Arms one capture per request, replacing any earlier request.
        void Request(Span<const WindowShotRequest> requests)
        {
            for (Slot& slot : m_slots)
            {
                slot.requested = false;
            }
            for (const WindowShotRequest& request : requests)
            {
                Slot& slot = SlotFor(request.window);
                slot.requested = true;
                slot.shot = WindowShot{};
                slot.shot.window = request.window;
                slot.shot.main = request.main;
                slot.shot.path = request.path;
                slot.capture->Request(request.path.AsView());
            }
        }

        /// The latest request's captures, in the order they were requested.
        [[nodiscard]] Array<WindowShot> Shots() const
        {
            Array<WindowShot> shots(*m_allocator);
            for (const Slot& slot : m_slots)
            {
                if (slot.requested)
                {
                    shots.PushBack(slot.shot);
                }
            }
            return shots;
        }

        /// Some requested capture is still waiting for its window.
        [[nodiscard]] bool Busy() const noexcept
        {
            for (const Slot& slot : m_slots)
            {
                if (slot.requested && slot.shot.state == WindowCaptureState::Pending)
                {
                    return true;
                }
            }
            return false;
        }

        /// From window `windowId`'s frame, after its UI has drawn: records the copy of its
        /// backbuffer when a capture of it is armed.
        void Record(foundation::rhi::Device& device, foundation::rhi::CommandEncoder& encoder, u32 windowId,
                    foundation::rhi::Texture* backbuffer, foundation::rhi::TextureFormat format, u32 width,
                    u32 height)
        {
            for (Slot& slot : m_slots)
            {
                if (slot.requested && slot.shot.window == windowId && slot.capture->Armed())
                {
                    if (!slot.capture->Record(device, encoder, backbuffer, format, width, height))
                    {
                        slot.shot.state = WindowCaptureState::Failed; // logged by the capture
                    }
                }
            }
        }

        /// The next update: writes the copies recorded last frame. A one-off, so it waits for
        /// the whole GPU once, then maps and writes each.
        void Complete(foundation::rhi::Device& device, IAllocator& allocator)
        {
            bool waited = false;
            for (Slot& slot : m_slots)
            {
                if (!slot.capture->Recorded())
                {
                    continue;
                }
                if (!waited)
                {
                    device.WaitIdle();
                    waited = true;
                }
                foundation::image::Image written;
                const Status saved = slot.capture->Complete(device, allocator, written);
                slot.shot.state = saved.IsOk() ? WindowCaptureState::Written : WindowCaptureState::Failed;
                slot.shot.width = written.Width();
                slot.shot.height = written.Height();
            }
        }

        /// A requested window that is no longer open never draws: its capture fails.
        void FailMissing(Span<const u32> openWindows)
        {
            for (Slot& slot : m_slots)
            {
                if (!slot.requested || slot.shot.state != WindowCaptureState::Pending)
                {
                    continue;
                }
                bool open = false;
                for (const u32 id : openWindows)
                {
                    open = open || id == slot.shot.window;
                }
                if (!open)
                {
                    slot.shot.state = WindowCaptureState::Failed;
                }
            }
        }

        /// Drops the readback buffers, while the device lives (the application's shutdown).
        void Release(foundation::rhi::Device& device)
        {
            for (Slot& slot : m_slots)
            {
                slot.capture->Release(device);
            }
        }

    private:
        struct Slot
        {
            u32 window = 0;
            bool requested = false;
            WindowShot shot;
            UniquePtr<engine::runtime::ScreenshotCapture> capture;
        };

        // One slot per window, kept across requests so its readback buffer is made once.
        Slot& SlotFor(u32 windowId)
        {
            for (Slot& slot : m_slots)
            {
                if (slot.window == windowId)
                {
                    return slot;
                }
            }
            Slot slot;
            slot.window = windowId;
            slot.capture = MakeUnique<engine::runtime::ScreenshotCapture>(*m_allocator);
            m_slots.PushBack(Move(slot));
            return m_slots[m_slots.Size() - 1];
        }

        IAllocator* m_allocator;
        Array<Slot> m_slots;
    };
}
