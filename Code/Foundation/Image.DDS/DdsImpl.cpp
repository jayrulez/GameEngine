// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// DDS container parse + write: the two header forms and the payload layout.

module;
#define _CRT_SECURE_NO_WARNINGS
#include "Core/Prelude.h"
#include <cstdio>
#include <cstring>
#include <string>

module foundation.image.dds;

import foundation.core;
import foundation.image;

using namespace foundation::core;

namespace foundation::image::dds
{
    namespace
    {
        constexpr u32 kMagic = 0x20534444u; // "DDS "
        constexpr u32 kHeaderSize = 124;
        constexpr u32 kPixelFormatSize = 32;
        constexpr u32 kDx10HeaderSize = 20;
        constexpr usize kFileHeaderBytes = 4 + kHeaderSize;

        // DDS_HEADER.dwFlags
        constexpr u32 kFlagCaps = 0x1;
        constexpr u32 kFlagHeight = 0x2;
        constexpr u32 kFlagWidth = 0x4;
        constexpr u32 kFlagPitch = 0x8;
        constexpr u32 kFlagPixelFormat = 0x1000;
        constexpr u32 kFlagMipMapCount = 0x20000;
        constexpr u32 kFlagLinearSize = 0x80000;
        // DDS_PIXELFORMAT.dwFlags
        constexpr u32 kPfAlphaPixels = 0x1;
        constexpr u32 kPfFourCC = 0x4;
        constexpr u32 kPfRgb = 0x40;
        constexpr u32 kPfLuminance = 0x20000;
        // dwCaps / dwCaps2
        constexpr u32 kCapsComplex = 0x8;
        constexpr u32 kCapsTexture = 0x1000;
        constexpr u32 kCapsMipMap = 0x400000;
        constexpr u32 kCaps2Cubemap = 0x200;
        constexpr u32 kCaps2CubemapAllFaces = 0xFE00;
        constexpr u32 kCaps2Volume = 0x200000;
        // DX10 header
        constexpr u32 kDimensionTexture2D = 3;
        constexpr u32 kDimensionTexture3D = 4;
        constexpr u32 kMiscTextureCube = 0x4;

        constexpr u32 FourCC(char a, char b, char c, char d) noexcept
        {
            return static_cast<u32>(static_cast<u8>(a)) | (static_cast<u32>(static_cast<u8>(b)) << 8) |
                   (static_cast<u32>(static_cast<u8>(c)) << 16) |
                   (static_cast<u32>(static_cast<u8>(d)) << 24);
        }
        constexpr u32 kFourCCDx10 = FourCC('D', 'X', '1', '0');

        // DXGI_FORMAT values the reader understands.
        enum Dxgi : u32
        {
            DXGI_R32G32B32A32_FLOAT = 2,
            DXGI_R16G16B16A16_FLOAT = 10,
            DXGI_R8G8B8A8_UNORM = 28,
            DXGI_R8G8B8A8_UNORM_SRGB = 29,
            DXGI_R8G8_UNORM = 49,
            DXGI_R8_UNORM = 61,
            DXGI_BC1_UNORM = 71,
            DXGI_BC1_UNORM_SRGB = 72,
            DXGI_BC2_UNORM = 74,
            DXGI_BC2_UNORM_SRGB = 75,
            DXGI_BC3_UNORM = 77,
            DXGI_BC3_UNORM_SRGB = 78,
            DXGI_BC4_UNORM = 80,
            DXGI_BC4_SNORM = 81,
            DXGI_BC5_UNORM = 83,
            DXGI_BC5_SNORM = 84,
            DXGI_B8G8R8A8_UNORM = 87,
            DXGI_B8G8R8A8_UNORM_SRGB = 91,
            DXGI_BC6H_UF16 = 95,
            DXGI_BC6H_SF16 = 96,
            DXGI_BC7_UNORM = 98,
            DXGI_BC7_UNORM_SRGB = 99,
        };

        [[nodiscard]] DdsFormat FromDxgi(u32 dxgi) noexcept
        {
            switch (dxgi)
            {
            case DXGI_R32G32B32A32_FLOAT: return DdsFormat::RGBA32F;
            case DXGI_R16G16B16A16_FLOAT: return DdsFormat::RGBA16F;
            case DXGI_R8G8B8A8_UNORM: return DdsFormat::RGBA8;
            case DXGI_R8G8B8A8_UNORM_SRGB: return DdsFormat::RGBA8Srgb;
            case DXGI_R8G8_UNORM: return DdsFormat::RG8;
            case DXGI_R8_UNORM: return DdsFormat::R8;
            case DXGI_BC1_UNORM: return DdsFormat::BC1;
            case DXGI_BC1_UNORM_SRGB: return DdsFormat::BC1Srgb;
            case DXGI_BC2_UNORM: return DdsFormat::BC2;
            case DXGI_BC2_UNORM_SRGB: return DdsFormat::BC2Srgb;
            case DXGI_BC3_UNORM: return DdsFormat::BC3;
            case DXGI_BC3_UNORM_SRGB: return DdsFormat::BC3Srgb;
            case DXGI_BC4_UNORM: return DdsFormat::BC4;
            case DXGI_BC4_SNORM: return DdsFormat::BC4Snorm;
            case DXGI_BC5_UNORM: return DdsFormat::BC5;
            case DXGI_BC5_SNORM: return DdsFormat::BC5Snorm;
            case DXGI_B8G8R8A8_UNORM: return DdsFormat::BGRA8;
            case DXGI_B8G8R8A8_UNORM_SRGB: return DdsFormat::BGRA8Srgb;
            case DXGI_BC6H_UF16: return DdsFormat::BC6HUf;
            case DXGI_BC6H_SF16: return DdsFormat::BC6HSf;
            case DXGI_BC7_UNORM: return DdsFormat::BC7;
            case DXGI_BC7_UNORM_SRGB: return DdsFormat::BC7Srgb;
            default: return DdsFormat::Unknown;
            }
        }

        [[nodiscard]] u32 ToDxgi(DdsFormat f) noexcept
        {
            switch (f)
            {
            case DdsFormat::RGBA32F: return DXGI_R32G32B32A32_FLOAT;
            case DdsFormat::RGBA16F: return DXGI_R16G16B16A16_FLOAT;
            case DdsFormat::RGBA8: return DXGI_R8G8B8A8_UNORM;
            case DdsFormat::RGBA8Srgb: return DXGI_R8G8B8A8_UNORM_SRGB;
            case DdsFormat::RG8: return DXGI_R8G8_UNORM;
            case DdsFormat::R8: return DXGI_R8_UNORM;
            case DdsFormat::BC1: return DXGI_BC1_UNORM;
            case DdsFormat::BC1Srgb: return DXGI_BC1_UNORM_SRGB;
            case DdsFormat::BC2: return DXGI_BC2_UNORM;
            case DdsFormat::BC2Srgb: return DXGI_BC2_UNORM_SRGB;
            case DdsFormat::BC3: return DXGI_BC3_UNORM;
            case DdsFormat::BC3Srgb: return DXGI_BC3_UNORM_SRGB;
            case DdsFormat::BC4: return DXGI_BC4_UNORM;
            case DdsFormat::BC4Snorm: return DXGI_BC4_SNORM;
            case DdsFormat::BC5: return DXGI_BC5_UNORM;
            case DdsFormat::BC5Snorm: return DXGI_BC5_SNORM;
            case DdsFormat::BGRA8: return DXGI_B8G8R8A8_UNORM;
            case DdsFormat::BGRA8Srgb: return DXGI_B8G8R8A8_UNORM_SRGB;
            case DdsFormat::BC6HUf: return DXGI_BC6H_UF16;
            case DdsFormat::BC6HSf: return DXGI_BC6H_SF16;
            case DdsFormat::BC7: return DXGI_BC7_UNORM;
            case DdsFormat::BC7Srgb: return DXGI_BC7_UNORM_SRGB;
            default: return 0;
            }
        }

        // The legacy pixel format block: a FourCC, or bit masks describing an uncompressed
        // texel. Only the layouts the engine reads map; anything else is Unknown.
        [[nodiscard]] DdsFormat FromLegacy(u32 pfFlags, u32 fourCC, u32 bitCount, u32 rMask,
                                           u32 gMask, u32 bMask, u32 aMask) noexcept
        {
            if ((pfFlags & kPfFourCC) != 0)
            {
                if (fourCC == FourCC('D', 'X', 'T', '1')) return DdsFormat::BC1;
                if (fourCC == FourCC('D', 'X', 'T', '2') || fourCC == FourCC('D', 'X', 'T', '3'))
                    return DdsFormat::BC2;
                if (fourCC == FourCC('D', 'X', 'T', '4') || fourCC == FourCC('D', 'X', 'T', '5'))
                    return DdsFormat::BC3;
                if (fourCC == FourCC('A', 'T', 'I', '1') || fourCC == FourCC('B', 'C', '4', 'U'))
                    return DdsFormat::BC4;
                if (fourCC == FourCC('B', 'C', '4', 'S')) return DdsFormat::BC4Snorm;
                if (fourCC == FourCC('A', 'T', 'I', '2') || fourCC == FourCC('B', 'C', '5', 'U'))
                    return DdsFormat::BC5;
                if (fourCC == FourCC('B', 'C', '5', 'S')) return DdsFormat::BC5Snorm;
                if (fourCC == 113) return DdsFormat::RGBA16F; // D3DFMT_A16B16G16R16F
                if (fourCC == 116) return DdsFormat::RGBA32F; // D3DFMT_A32B32G32R32F
                return DdsFormat::Unknown;
            }
            if ((pfFlags & kPfRgb) != 0 && bitCount == 32)
            {
                const bool alpha = (pfFlags & kPfAlphaPixels) != 0 && aMask == 0xFF000000u;
                if (rMask == 0x000000FFu && gMask == 0x0000FF00u && bMask == 0x00FF0000u &&
                    (alpha || aMask == 0))
                {
                    return DdsFormat::RGBA8;
                }
                if (rMask == 0x00FF0000u && gMask == 0x0000FF00u && bMask == 0x000000FFu &&
                    (alpha || aMask == 0))
                {
                    return DdsFormat::BGRA8; // A8R8G8B8 / X8R8G8B8: BGRA in memory
                }
                return DdsFormat::Unknown;
            }
            if ((pfFlags & kPfLuminance) != 0 && bitCount == 8 && rMask == 0xFFu)
            {
                return DdsFormat::R8;
            }
            if ((pfFlags & kPfRgb) != 0 && bitCount == 16 && rMask == 0x00FFu && gMask == 0xFF00u)
            {
                return DdsFormat::RG8;
            }
            return DdsFormat::Unknown;
        }

        [[nodiscard]] u32 ReadU32(const u8* p) noexcept
        {
            return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                   (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
        }
        void WriteU32(Array<u8>& out, u32 v)
        {
            out.PushBack(static_cast<u8>(v & 0xFFu));
            out.PushBack(static_cast<u8>((v >> 8) & 0xFFu));
            out.PushBack(static_cast<u8>((v >> 16) & 0xFFu));
            out.PushBack(static_cast<u8>((v >> 24) & 0xFFu));
        }
    } // namespace

    bool IsDds(Span<const u8> bytes) noexcept
    {
        return bytes.Size() >= 4 && ReadU32(bytes.Data()) == kMagic;
    }

    bool IsDdsFile(StringView path) noexcept
    {
        const std::string cPath(reinterpret_cast<const char*>(path.Data()), path.Size());
        FILE* file = std::fopen(cPath.c_str(), "rb");
        if (file == nullptr)
        {
            return false;
        }
        u8 magic[4] = {};
        const usize got = std::fread(magic, 1, 4, file);
        std::fclose(file);
        return got == 4 && IsDds(Span<const u8>(magic, 4));
    }

    namespace
    {
        // The header parse LoadDds and ParseDdsHeader share: the facts and where the payload starts.
        Status ParseHeader(Span<const u8> bytes, DdsHeader& out, usize& payloadStart)
        {
            if (bytes.Size() < kFileHeaderBytes || !IsDds(bytes))
            {
                return ErrorCode::InvalidArgument;
            }
            const u8* h = bytes.Data() + 4;
            if (ReadU32(h) != kHeaderSize || ReadU32(h + 72) != kPixelFormatSize)
            {
                return ErrorCode::InvalidArgument;
            }
            const u32 flags = ReadU32(h + 4);
            const u32 height = ReadU32(h + 8);
            const u32 width = ReadU32(h + 12);
            const u32 mipCount = ReadU32(h + 24);
            const u32 pfFlags = ReadU32(h + 76);
            const u32 fourCC = ReadU32(h + 80);
            const u32 bitCount = ReadU32(h + 84);
            const u32 rMask = ReadU32(h + 88);
            const u32 gMask = ReadU32(h + 92);
            const u32 bMask = ReadU32(h + 96);
            const u32 aMask = ReadU32(h + 100);
            const u32 caps2 = ReadU32(h + 108);
            DdsHeader header;
            header.width = width;
            header.height = height;
            header.mipLevels = ((flags & kFlagMipMapCount) != 0 && mipCount > 0) ? mipCount : 1u;
            payloadStart = kFileHeaderBytes;
            bool cube = (caps2 & kCaps2Cubemap) != 0;
            bool volume = (caps2 & kCaps2Volume) != 0;
            u32 arraySize = 1;
            if ((pfFlags & kPfFourCC) != 0 && fourCC == kFourCCDx10)
            {
                if (bytes.Size() < kFileHeaderBytes + kDx10HeaderSize)
                {
                    return ErrorCode::InvalidArgument;
                }
                const u8* x = bytes.Data() + kFileHeaderBytes;
                const u32 dxgi = ReadU32(x);
                const u32 dimension = ReadU32(x + 4);
                const u32 misc = ReadU32(x + 8);
                arraySize = ReadU32(x + 12);
                header.format = FromDxgi(dxgi);
                header.colorSpaceKnown = true;
                cube = cube || (misc & kMiscTextureCube) != 0;
                volume = volume || dimension == kDimensionTexture3D;
                if (dimension != kDimensionTexture2D && dimension != kDimensionTexture3D)
                {
                    return ErrorCode::NotSupported; // 1D textures: nothing in the engine wants one
                }
                payloadStart += kDx10HeaderSize;
            }
            else
            {
                header.format = FromLegacy(pfFlags, fourCC, bitCount, rMask, gMask, bMask, aMask);
                header.colorSpaceKnown = false;
            }
            if (volume)
            {
                return ErrorCode::NotSupported;
            }
            if (header.format == DdsFormat::Unknown || width == 0 || height == 0 || arraySize == 0)
            {
                return ErrorCode::NotSupported;
            }
            if (cube && (caps2 & kCaps2Cubemap) != 0 &&
                (caps2 & kCaps2CubemapAllFaces) != kCaps2CubemapAllFaces)
            {
                return ErrorCode::NotSupported; // a partial cubemap has no fixed layout
            }
            header.cubemap = cube;
            header.arrayLayers = cube ? arraySize * 6u : arraySize;
            out = header;
            return ErrorCode::Ok;
        }
    }

    Status ParseDdsHeader(Span<const u8> bytes, DdsHeader& out)
    {
        usize payloadStart = 0;
        return ParseHeader(bytes, out, payloadStart);
    }

    Status ReadDdsHeader(StringView path, DdsHeader& out)
    {
        const std::string cPath(reinterpret_cast<const char*>(path.Data()), path.Size());
        FILE* file = std::fopen(cPath.c_str(), "rb");
        if (file == nullptr)
        {
            return ErrorCode::NotFound;
        }
        u8 head[kDdsHeaderBytes] = {};
        const usize got = std::fread(head, 1, sizeof(head), file);
        std::fclose(file);
        return ParseDdsHeader(Span<const u8>(head, got), out);
    }

    Status LoadDds(Span<const u8> bytes, DdsImage& out)
    {
        DdsHeader header;
        usize payloadStart = 0;
        const Status parsed = ParseHeader(bytes, header, payloadStart);
        if (!parsed.IsOk())
        {
            return parsed;
        }
        DdsImage dds;
        dds.width = header.width;
        dds.height = header.height;
        dds.mipLevels = header.mipLevels;
        dds.arrayLayers = header.arrayLayers;
        dds.cubemap = header.cubemap;
        dds.format = header.format;
        dds.colorSpaceKnown = header.colorSpaceKnown;
        const usize payload = dds.LayerSize() * dds.arrayLayers;
        if (bytes.Size() < payloadStart + payload)
        {
            return ErrorCode::InvalidArgument; // truncated
        }
        dds.data.Resize(payload);
        if (payload > 0)
        {
            std::memcpy(dds.data.Data(), bytes.Data() + payloadStart, payload);
        }
        out = Move(dds);
        return ErrorCode::Ok;
    }

    Status WriteDds(const DdsImage& dds, Array<u8>& out)
    {
        if (dds.format == DdsFormat::Unknown || dds.width == 0 || dds.height == 0 ||
            dds.arrayLayers == 0 || (dds.cubemap && dds.arrayLayers % 6u != 0))
        {
            return ErrorCode::InvalidArgument;
        }
        const usize payload = dds.LayerSize() * dds.arrayLayers;
        if (dds.data.Size() < payload)
        {
            return ErrorCode::InvalidArgument;
        }
        out.Clear();
        out.Reserve(kFileHeaderBytes + kDx10HeaderSize + payload);
        WriteU32(out, kMagic);
        u32 flags = kFlagCaps | kFlagHeight | kFlagWidth | kFlagPixelFormat;
        flags |= IsBlockCompressed(dds.format) ? kFlagLinearSize : kFlagPitch;
        if (dds.mipLevels > 1)
        {
            flags |= kFlagMipMapCount;
        }
        WriteU32(out, kHeaderSize);
        WriteU32(out, flags);
        WriteU32(out, dds.height);
        WriteU32(out, dds.width);
        // pitchOrLinearSize: the level-0 bytes for compressed, the row pitch for uncompressed.
        WriteU32(out, IsBlockCompressed(dds.format)
                          ? static_cast<u32>(dds.LevelSize(0))
                          : dds.width * BytesPerPixel(dds.format));
        WriteU32(out, 1); // depth
        WriteU32(out, dds.mipLevels);
        for (u32 i = 0; i < 11; ++i)
        {
            WriteU32(out, 0); // reserved1
        }
        // DDS_PIXELFORMAT: FourCC "DX10".
        WriteU32(out, kPixelFormatSize);
        WriteU32(out, kPfFourCC);
        WriteU32(out, kFourCCDx10);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        WriteU32(out, 0);
        u32 caps = kCapsTexture;
        if (dds.mipLevels > 1)
        {
            caps |= kCapsMipMap | kCapsComplex;
        }
        if (dds.cubemap)
        {
            caps |= kCapsComplex;
        }
        WriteU32(out, caps);
        WriteU32(out, dds.cubemap ? (kCaps2Cubemap | kCaps2CubemapAllFaces) : 0u);
        WriteU32(out, 0); // caps3
        WriteU32(out, 0); // caps4
        WriteU32(out, 0); // reserved2
        // DDS_HEADER_DXT10.
        WriteU32(out, ToDxgi(dds.format));
        WriteU32(out, kDimensionTexture2D);
        WriteU32(out, dds.cubemap ? kMiscTextureCube : 0u);
        WriteU32(out, dds.cubemap ? dds.arrayLayers / 6u : dds.arrayLayers);
        WriteU32(out, 0); // miscFlags2: alpha mode unknown
        const usize at = out.Size();
        out.Resize(at + payload);
        if (payload > 0)
        {
            std::memcpy(out.Data() + at, dds.data.Data(), payload);
        }
        return ErrorCode::Ok;
    }

    Status LoadDdsAsImage(Span<const u8> bytes, Image& out)
    {
        DdsImage dds;
        const Status loaded = LoadDds(bytes, dds);
        if (!loaded.IsOk())
        {
            return loaded;
        }
        return DecodeLevel(dds, 0, 0, out);
    }
}
