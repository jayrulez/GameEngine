// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Engine::DefaultApp - the `engine.defaultapp:screenshot` partition: the backbuffer -> PNG
// capture behind DefaultApplication::CaptureScreenshot, the F11 hotkey and the --screenshot
// flags. Legacy Sedulous had the request and the GPU copy and stopped there (the readback buffer
// was filled and nothing mapped it); this is the whole path: arm, copy the presented backbuffer
// inside the frame (RenderTarget -> CopySrc -> RenderTarget, so the host's own Present
// transition still holds), then after the GPU is done map the 256-byte-aligned rows into a
// tight RGBA8 image, swizzle BGRA surfaces, and write the PNG through foundation.image.io.
//
// Two halves so each is testable on its own: Record() needs a live encoder and a copy-capable
// backbuffer (Vulkan surfaces expose TRANSFER_SRC when the driver allows; the WebGPU swapchain
// asks for CopySrc when the surface offers it); Complete() needs only the mapped bytes.
module;
#include "Core/Prelude.h"
#include "Core/Log/Log.h"

export module engine.defaultapp:screenshot;

import foundation.core;
import foundation.rhi;
import foundation.image;
import foundation.image.io;

using namespace foundation::core;
namespace rhi = foundation::rhi;
namespace image = foundation::image;

export namespace engine::runtime
{
    /// The --screenshot flags: capture to `path` at rendered frame `frame` (1-based) or, when
    /// `afterSeconds` is set, at the first frame past that many seconds of run time (frame
    /// rate independent - the form a "screenshot after 15 seconds" wants); exit once it is
    /// written when asked. Empty path = no capture.
    struct ScreenshotOptions
    {
        String path;
        u32 frame = 30;           // give the scene a few frames to load and settle
        f32 afterSeconds = 0.0f;  // > 0 wins over `frame`
        bool exitAfter = false;   // --screenshot-exit: quit once the file is written
        u32 count = 1;            // --screenshot-count: this many consecutive frames from the due one
        [[nodiscard]] bool Requested() const noexcept { return !path.IsEmpty(); }
        /// The file for the `index`-th frame of a run: the path itself for one frame, else the path
        /// with "-<index>" before its extension ("shot.png" -> "shot-0.png", "shot-1.png", ...).
        [[nodiscard]] String PathFor(u32 index) const
        {
            if (count <= 1)
            {
                return path;
            }
            const StringView p = path.AsView();
            usize dot = p.Size();
            for (usize i = p.Size(); i > 0; --i)
            {
                if (p[i - 1] == u8'.')
                {
                    dot = i - 1;
                    break;
                }
                if (p[i - 1] == u8'/' || p[i - 1] == u8'\\')
                {
                    break;
                }
            }
            return Format(u8"{}-{}{}", p.SubStr(0, dot), index, p.SubStr(dot, p.Size() - dot));
        }
        /// Whether this frame (the `frames`-th rendered, at `seconds` of run time) is the one.
        [[nodiscard]] bool Due(u64 frames, f32 seconds) const noexcept
        {
            return afterSeconds > 0.0f ? (seconds >= afterSeconds) : (frames == frame);
        }
    };

    /// Parses `--screenshot <path>`, `--screenshot-frame <n>`, `--screenshot-after <seconds>`,
    /// `--screenshot-count <n>` (consecutive frames: measuring frame-to-frame change, TAA's jitter one)
    /// and `--screenshot-exit` out of a command line, ignoring everything else (the other flags have
    /// their own readers).
    [[nodiscard]] inline ScreenshotOptions ScreenshotOptionsFromArguments(int argc, char** argv)
    {
        ScreenshotOptions options;
        for (int i = 1; i < argc; ++i)
        {
            const StringView arg(reinterpret_cast<const utf8char*>(argv[i]));
            if (arg == u8"--screenshot" && i + 1 < argc)
            {
                options.path = String(StringView(reinterpret_cast<const utf8char*>(argv[++i])));
            }
            else if (arg == u8"--screenshot-frame" && i + 1 < argc)
            {
                u32 frame = 0;
                for (const char* c = argv[++i]; *c >= '0' && *c <= '9'; ++c)
                {
                    frame = frame * 10u + static_cast<u32>(*c - '0');
                }
                options.frame = (frame > 0) ? frame : 1u;
            }
            else if (arg == u8"--screenshot-after" && i + 1 < argc)
            {
                f32 seconds = 0.0f, scale = 1.0f;
                bool fraction = false;
                for (const char* c = argv[++i]; *c != '\0'; ++c)
                {
                    if (*c == '.' && !fraction)
                    {
                        fraction = true;
                    }
                    else if (*c >= '0' && *c <= '9')
                    {
                        if (fraction)
                        {
                            scale *= 0.1f;
                            seconds += static_cast<f32>(*c - '0') * scale;
                        }
                        else
                        {
                            seconds = seconds * 10.0f + static_cast<f32>(*c - '0');
                        }
                    }
                    else
                    {
                        break;
                    }
                }
                options.afterSeconds = seconds;
            }
            else if (arg == u8"--screenshot-count" && i + 1 < argc)
            {
                u32 count = 0;
                for (const char* c = argv[++i]; *c >= '0' && *c <= '9'; ++c)
                {
                    count = count * 10u + static_cast<u32>(*c - '0');
                }
                options.count = (count > 0) ? count : 1u;
            }
            else if (arg == u8"--screenshot-exit")
            {
                options.exitAfter = true;
            }
        }
        return options;
    }

    class ScreenshotCapture
    {
    public:
        static constexpr u32 kRowAlignment = 256; // Vulkan + WebGPU buffer-copy row pitch

        ~ScreenshotCapture() { Release(); }

        /// Arm: the next Record() copies the frame to `path`.
        void Request(StringView path)
        {
            m_path = String(path);
            m_armed = true;
        }
        [[nodiscard]] bool Armed() const noexcept { return m_armed; }
        /// A copy was recorded and awaits Complete() once the GPU has run it.
        [[nodiscard]] bool Recorded() const noexcept { return m_recorded; }
        [[nodiscard]] StringView Path() const noexcept { return m_path.AsView(); }

        /// Whether a surface format can be written as an 8-bit PNG: the 8-bit RGBA/BGRA surfaces
        /// straight through, and RGBA16Float - the viewports' LINEAR target (the tonemap and the
        /// UI both write linear there) - encoded to sRGB bytes.
        [[nodiscard]] static bool CanCapture(rhi::TextureFormat format) noexcept
        {
            switch (format)
            {
            case rhi::TextureFormat::RGBA8Unorm:
            case rhi::TextureFormat::RGBA8UnormSrgb:
            case rhi::TextureFormat::BGRA8Unorm:
            case rhi::TextureFormat::BGRA8UnormSrgb:
            case rhi::TextureFormat::RGBA16Float:
                return true;
            default:
                return false;
            }
        }
        [[nodiscard]] static u32 BytesPerPixel(rhi::TextureFormat format) noexcept
        {
            return format == rhi::TextureFormat::RGBA16Float ? 8u : 4u;
        }
        [[nodiscard]] static bool IsBgra(rhi::TextureFormat format) noexcept
        {
            return format == rhi::TextureFormat::BGRA8Unorm ||
                   format == rhi::TextureFormat::BGRA8UnormSrgb;
        }

        /// Records the copy into `encoder` while `backbuffer` sits in `state` - RenderTarget for
        /// a presented backbuffer (the state the host hands the frame over in and expects back),
        /// ShaderRead for an editor viewport's finished colour target - and leaves it there.
        /// Disarms; false when nothing was armed, the format cannot be captured, or the readback
        /// buffer could not be made - each logged, so a silent no-op never passes for a
        /// screenshot. `originX` / `originY` capture a sub-rectangle of that size from there (a
        /// letterboxed game's image without its bars). The PNG is the captured pixels: a view drawn
        /// smaller than its content's resolution is written at the size it was drawn (scaling it
        /// back up only blurred it into something that looked like a font problem).
        bool Record(rhi::Device& device, rhi::CommandEncoder& encoder, rhi::Texture* backbuffer,
                    rhi::TextureFormat format, u32 width, u32 height,
                    rhi::ResourceState state = rhi::ResourceState::RenderTarget, u32 originX = 0,
                    u32 originY = 0)
        {
            if (!m_armed)
            {
                return false;
            }
            m_armed = false;
            if (backbuffer == nullptr || width == 0 || height == 0)
            {
                LOG_ERROR(u8"Screenshot", u8"no backbuffer to capture");
                return false;
            }
            if (!CanCapture(format))
            {
                LOG_ERROR(u8"Screenshot", u8"backbuffer format {} is not an 8-bit RGBA/BGRA surface",
                          static_cast<u32>(format));
                return false;
            }
            const u32 bytesPerRow =
                (width * BytesPerPixel(format) + (kRowAlignment - 1u)) & ~(kRowAlignment - 1u);
            const u64 needed = static_cast<u64>(bytesPerRow) * height;
            if (m_readback == nullptr || m_readbackSize < needed)
            {
                Release(device);
                rhi::BufferDesc desc{};
                desc.size = needed;
                desc.usage = rhi::BufferUsage::CopyDst;
                desc.memory = rhi::MemoryLocation::GpuToCpu;
                desc.label = u8"screenshot.readback";
                if (!device.CreateBuffer(desc, m_readback).IsOk() || m_readback == nullptr)
                {
                    LOG_ERROR(u8"Screenshot", u8"could not create the {} byte readback buffer", needed);
                    m_readback = nullptr;
                    return false;
                }
                m_readbackSize = needed;
            }
            encoder.TransitionTexture(backbuffer, state, rhi::ResourceState::CopySrc);
            rhi::BufferTextureCopyRegion region;
            region.bytesPerRow = bytesPerRow;
            region.rowsPerImage = height;
            region.textureOrigin = rhi::Origin3D{originX, originY, 0};
            region.textureExtent = rhi::Extent3D{width, height, 1};
            encoder.CopyTextureToBuffer(backbuffer, m_readback, region);
            encoder.TransitionTexture(backbuffer, rhi::ResourceState::CopySrc, state);
            m_width = width;
            m_height = height;
            m_bytesPerRow = bytesPerRow;
            m_format = format;
            m_recorded = true;
            return true;
        }

        /// Copies the aligned rows the GPU wrote into a tight RGBA8 image: BGRA swizzled, a
        /// 16-bit float texel clamped to [0, 1] and sRGB encoded, as the editor encodes it for
        /// the screen (the float target holds linear values; alpha is coverage and stays linear).
        static void UnpackRows(const u8* mapped, u32 bytesPerRow, u32 width, u32 height,
                               rhi::TextureFormat format, Span<u8> outRgba)
        {
            const bool bgra = IsBgra(format);
            const bool half = format == rhi::TextureFormat::RGBA16Float;
            const u32 bytesPerPixel = BytesPerPixel(format);
            for (u32 y = 0; y < height; ++y)
            {
                const u8* src = mapped + static_cast<usize>(y) * bytesPerRow;
                u8* dst = outRgba.Data() + static_cast<usize>(y) * width * 4u;
                for (u32 x = 0; x < width; ++x)
                {
                    const u8* s = src + static_cast<usize>(x) * bytesPerPixel;
                    u8* d = dst + static_cast<usize>(x) * 4u;
                    if (half)
                    {
                        for (u32 c = 0; c < 4; ++c)
                        {
                            u16 bits;
                            MemCopy(&bits, s + c * 2u, sizeof(bits));
                            if (c == 3)
                            {
                                d[c] = image::HalfToUnorm8(bits); // coverage: linear
                                continue;
                            }
                            const f32 linear = Clamp(image::HalfToFloat(bits), 0.0f, 1.0f);
                            d[c] = static_cast<u8>(LinearToSrgb(linear) * 255.0f + 0.5f);
                        }
                        continue;
                    }
                    d[0] = bgra ? s[2] : s[0];
                    d[1] = s[1];
                    d[2] = bgra ? s[0] : s[2];
                    d[3] = s[3];
                }
            }
        }

        /// After the GPU has finished the recorded copy (the caller waits - a fence or WaitIdle):
        /// maps the readback, unpacks it (through `allocator`, the caller's) into `outImage`
        /// (RGBA8) and writes the PNG at Path(). Consumes the recording. The image is returned as
        /// well so a test can look at it.
        Status Complete(rhi::Device& device, IAllocator& allocator, image::Image& outImage)
        {
            if (!m_recorded)
            {
                return Status{ErrorCode::Internal}; // nothing recorded
            }
            m_recorded = false;
            const u8* mapped = static_cast<const u8*>(m_readback->Map());
            if (mapped == nullptr)
            {
                LOG_ERROR(u8"Screenshot", u8"could not map the readback buffer");
                Release(device);
                return Status{ErrorCode::Unknown};
            }
            Array<u8> rgba(allocator);
            rgba.Resize(static_cast<usize>(m_width) * m_height * 4u);
            UnpackRows(mapped, m_bytesPerRow, m_width, m_height, m_format, Span<u8>{rgba.Data(), rgba.Size()});
            m_readback->Unmap();
            const u32 width = m_width;
            const u32 height = m_height;
            outImage = image::Image(width, height, image::PixelFormat::RGBA8,
                                    Span<const u8>{rgba.Data(), rgba.Size()});
            const Status saved =
                image::io::SaveImage(outImage, m_path.AsView(), image::io::ImageFileFormat::PNG);
            if (!saved.IsOk())
            {
                LOG_ERROR(u8"Screenshot", u8"could not write '{}'", m_path.AsView());
            }
            else
            {
                LOG_INFO(u8"Screenshot", u8"wrote '{}' ({}x{})", m_path.AsView(), width, height);
            }
            return saved;
        }

        /// Drops the readback buffer (device teardown).
        void Release(rhi::Device& device)
        {
            if (m_readback != nullptr)
            {
                device.DestroyBuffer(m_readback);
            }
            m_readback = nullptr;
            m_readbackSize = 0;
            m_recorded = false;
        }

    private:
        void Release()
        {
            // No device here: a capture left recorded at destruction was never completed; the
            // owning application releases through Release(device) in its shutdown.
            m_recorded = false;
        }

        String m_path;
        bool m_armed = false;
        bool m_recorded = false;
        rhi::Buffer* m_readback = nullptr;
        u64 m_readbackSize = 0;
        u32 m_width = 0, m_height = 0, m_bytesPerRow = 0;
        rhi::TextureFormat m_format = rhi::TextureFormat::RGBA8Unorm;
    };
}
