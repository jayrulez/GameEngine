// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Concrete image loading and saving via stb_image / stb_image_write.
/// No abstract loader or writer: a direct stb dependency.
/// Works with foundation::image::Image directly.

module;
#include "Core/Prelude.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image.h"
#include "stb_image_write.h"

export module foundation.image.io;

import foundation.core;
import foundation.image;
import foundation.image.dds;

using namespace foundation::core;

export namespace foundation::image::io
{

    /// File format for saving.
    enum class ImageFileFormat : u32
    {
        PNG,
        JPG,
        BMP
    };

    /// Load an image from a memory buffer.
    [[nodiscard]] inline Status LoadImageFromMemory(Span<const u8> buffer, Image& out);

    /// Load an image from a file path. Returns RGBA8 for LDR, RGBA32F for HDR. A DDS file
    /// (sniffed by magic, not extension) hands back its level 0 decoded.
    [[nodiscard]] inline Status LoadImage(StringView path, Image& out)
    {
        const std::string cPath(reinterpret_cast<const char*>(path.Data()), path.Size());
        {
            // A DDS is sniffed by its magic (a four-byte probe; stb still streams a PNG/JPG
            // from the path itself); an unreadable file falls through for stb to report.
            if (dds::IsDdsFile(path))
            {
                Result<Array<byte>> bytes = ReadFile(path);
                if (!bytes.HasValue())
                {
                    return bytes.Error();
                }
                return dds::LoadDdsAsImage(
                    Span<const u8>(reinterpret_cast<const u8*>(bytes.Value().Data()),
                                   bytes.Value().Size()),
                    out);
            }
        }
        int x = 0, y = 0, channels = 0;
        constexpr int desired = 4;

        bool isHDR = stbi_is_hdr(cPath.c_str()) != 0;
        void* data = nullptr;
        if (isHDR)
            data = stbi_loadf(cPath.c_str(), &x, &y, &channels, desired);
        else
            data = stbi_load(cPath.c_str(), &x, &y, &channels, desired);

        if (!data)
            return ErrorCode::Unknown;

        usize dataSize = isHDR ? static_cast<usize>(x) * y * desired * sizeof(float)
                               : static_cast<usize>(x) * y * desired;

        PixelFormat fmt = isHDR ? PixelFormat::RGBA32F : PixelFormat::RGBA8;
        out = Image(static_cast<u32>(x), static_cast<u32>(y), fmt,
                    Span<const u8>(static_cast<const u8*>(data), dataSize));
        out.SetColorSpace(isHDR ? ImageColorSpace::Linear : ImageColorSpace::Srgb);
        stbi_image_free(data);
        return ErrorCode::Ok;
    }

    inline Status LoadImageFromMemory(Span<const u8> buffer, Image& out)
    {
        if (dds::IsDds(buffer))
        {
            return dds::LoadDdsAsImage(buffer, out); // level 0, decoded
        }
        int x = 0, y = 0, channels = 0;
        constexpr int desired = 4;

        bool isHDR = stbi_is_hdr_from_memory(buffer.Data(), static_cast<int>(buffer.Size())) != 0;
        void* data = nullptr;
        if (isHDR)
            data = stbi_loadf_from_memory(buffer.Data(), static_cast<int>(buffer.Size()), &x, &y,
                                          &channels, desired);
        else
            data = stbi_load_from_memory(buffer.Data(), static_cast<int>(buffer.Size()), &x, &y,
                                         &channels, desired);

        if (!data)
            return ErrorCode::Unknown;

        usize dataSize = isHDR ? static_cast<usize>(x) * y * desired * sizeof(float)
                               : static_cast<usize>(x) * y * desired;

        PixelFormat fmt = isHDR ? PixelFormat::RGBA32F : PixelFormat::RGBA8;
        out = Image(static_cast<u32>(x), static_cast<u32>(y), fmt,
                    Span<const u8>(static_cast<const u8*>(data), dataSize));
        out.SetColorSpace(isHDR ? ImageColorSpace::Linear : ImageColorSpace::Srgb);
        stbi_image_free(data);
        return ErrorCode::Ok;
    }

    /// Load a single-channel 16-bit image (a heightmap) at full 16-bit precision. Produces an
    /// Image with PixelFormat::R16 (2 bytes per pixel, one channel). An 8-bit source is promoted
    /// to 16-bit by stb; a multi-channel source collapses to luminance. This is the heightfield
    /// import path; the general LoadImage down-samples 16-bit to 8-bit via stbi_load.
    [[nodiscard]] inline Status LoadImage16FromMemory(Span<const u8> buffer, Image& out)
    {
        int x = 0, y = 0, channels = 0;
        constexpr int desired = 1;
        stbi_us* data = stbi_load_16_from_memory(buffer.Data(), static_cast<int>(buffer.Size()), &x,
                                                 &y, &channels, desired);
        if (!data)
        {
            return ErrorCode::Unknown;
        }
        const usize dataSize = static_cast<usize>(x) * static_cast<usize>(y) * sizeof(stbi_us);
        out = Image(static_cast<u32>(x), static_cast<u32>(y), PixelFormat::R16,
                    Span<const u8>(reinterpret_cast<const u8*>(data), dataSize));
        out.SetColorSpace(ImageColorSpace::Linear); // heights are data, not color
        stbi_image_free(data);
        return ErrorCode::Ok;
    }

    /// Load a single-channel 16-bit image from a file path (see LoadImage16FromMemory).
    [[nodiscard]] inline Status LoadImage16(StringView path, Image& out)
    {
        const std::string cPath(reinterpret_cast<const char*>(path.Data()), path.Size());
        int x = 0, y = 0, channels = 0;
        constexpr int desired = 1;
        stbi_us* data = stbi_load_16(cPath.c_str(), &x, &y, &channels, desired);
        if (!data)
        {
            return ErrorCode::Unknown;
        }
        const usize dataSize = static_cast<usize>(x) * static_cast<usize>(y) * sizeof(stbi_us);
        out = Image(static_cast<u32>(x), static_cast<u32>(y), PixelFormat::R16,
                    Span<const u8>(reinterpret_cast<const u8*>(data), dataSize));
        out.SetColorSpace(ImageColorSpace::Linear);
        stbi_image_free(data);
        return ErrorCode::Ok;
    }

    /// Save an image to a file. Only supports 8-bit formats (R8, RG8, RGB8, RGBA8).
    [[nodiscard]] inline Status SaveImage(const Image& image, StringView path,
                                          ImageFileFormat format, i32 jpgQuality = 90)
    {
        if (image.Width() == 0 || image.Height() == 0)
            return ErrorCode::Unknown;

        switch (image.Format())
        {
        case PixelFormat::R8:
        case PixelFormat::RG8:
        case PixelFormat::RGB8:
        case PixelFormat::RGBA8:
            break;
        default:
            return ErrorCode::Unknown;
        }

        const std::string cPath(reinterpret_cast<const char*>(path.Data()), path.Size());
        int w = static_cast<int>(image.Width());
        int h = static_cast<int>(image.Height());
        int ch = static_cast<int>(ChannelCount(image.Format()));
        const void* data = image.PixelData().Data();

        int ok = 0;
        switch (format)
        {
        case ImageFileFormat::PNG:
            ok = stbi_write_png(cPath.c_str(), w, h, ch, data, w * ch);
            break;
        case ImageFileFormat::JPG:
            ok = stbi_write_jpg(cPath.c_str(), w, h, ch, data, jpgQuality);
            break;
        case ImageFileFormat::BMP:
            ok = stbi_write_bmp(cPath.c_str(), w, h, ch, data);
            break;
        }
        return ok != 0 ? ErrorCode::Ok : ErrorCode::Unknown;
    }

} // namespace foundation::image::io
