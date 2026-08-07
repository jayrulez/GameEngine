/// Image - owns a CPU-side pixel buffer with manipulation methods.
/// Implements ImageData so it can be passed to anything accepting the base type.
/// Ported from Sedulous.Images.Image.

module;
#include "Draconic.Foundation/Prelude.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

export module draconic.image:image;

import draconic.foundation;
import :pixel_format;
import :image_data;

using namespace draconic::foundation;

export namespace draconic::image
{

    // Pixel access uses the engine's packed byte color, foundation::Color32 (the image
    // library previously defined its own duplicate `Color` - unified away).

    /// Image that owns a pixel buffer. Inherits ImageData for polymorphic use.
    /// Supports pixel access, flips, format conversion, and procedural factories.
    class Image : public ImageData
    {
    public:
        Image() = default;

        Image(u32 w, u32 h, PixelFormat fmt, Span<const u8> srcData = {})
            : m_width(w), m_height(h), m_format(fmt)
        {
            usize needed = DataSize();
            m_data.Resize(needed);
            if (srcData.Size() >= needed)
                std::memcpy(m_data.Data(), srcData.Data(), needed);
            else
                Clear();
        }

        Image(const Image&) = default;
        Image(Image&&) noexcept = default;
        Image& operator=(const Image&) = default;
        Image& operator=(Image&&) noexcept = default;

        // ---- ImageData interface ----
        [[nodiscard]] u32 Width() const override { return m_width; }
        [[nodiscard]] u32 Height() const override { return m_height; }
        [[nodiscard]] PixelFormat Format() const override { return m_format; }
        [[nodiscard]] ImageColorSpace ColorSpace() const override { return m_colorSpace; }
        [[nodiscard]] Span<const u8> PixelData() const override
        {
            return {m_data.Data(), m_data.Size()};
        }

        // ---- Mutable access ----
        [[nodiscard]] Span<u8> PixelDataMut() { return {m_data.Data(), m_data.Size()}; }
        [[nodiscard]] u32 PixelCount() const { return m_width * m_height; }
        [[nodiscard]] usize DataSize() const
        {
            return static_cast<usize>(PixelCount()) * BytesPerPixel(m_format);
        }
        void SetColorSpace(ImageColorSpace cs) { m_colorSpace = cs; }

        /// Replace dimensions, format, and pixel data in-place (hot-reload).
        void ReplaceData(u32 w, u32 h, PixelFormat fmt, Span<const u8> src)
        {
            m_width = w;
            m_height = h;
            m_format = fmt;
            usize needed = DataSize();
            m_data.Resize(needed);
            usize copy = Min(needed, src.Size());
            if (copy > 0)
                std::memcpy(m_data.Data(), src.Data(), copy);
            if (copy < needed)
                std::memset(m_data.Data() + copy, 0, needed - copy);
        }

        // ---- Pixel access ----

        void Clear()
        {
            if (draconic::image::HasAlpha(m_format))
                FillColor(Color32::Transparent);
            else
                std::memset(m_data.Data(), 0, m_data.Size());
        }

        /// Clear the image to a specific color.
        void Clear(Color32 color) { FillColor(color); }

        /// Bytes per pixel for a format (static helper mirroring Sedulous's API).
        [[nodiscard]] static i32 GetBytesPerPixel(PixelFormat format)
        {
            return static_cast<i32>(BytesPerPixel(format));
        }

        /// Whether this image's format carries an alpha channel.
        [[nodiscard]] bool HasAlpha() const { return draconic::image::HasAlpha(m_format); }
        /// Number of channels in this image's pixel format.
        [[nodiscard]] i32 GetChannelCount() const
        {
            return static_cast<i32>(draconic::image::ChannelCount(m_format));
        }

        void FillColor(Color32 c)
        {
            u32 bpp = BytesPerPixel(m_format);
            for (usize i = 0; i < m_data.Size(); i += bpp)
            {
                switch (m_format)
                {
                case PixelFormat::R8:
                    m_data[i] = static_cast<u8>((c.r + c.g + c.b) / 3);
                    break;
                case PixelFormat::RG8:
                    m_data[i] = c.r;
                    m_data[i + 1] = c.g;
                    break;
                case PixelFormat::RGB8:
                    m_data[i] = c.r;
                    m_data[i + 1] = c.g;
                    m_data[i + 2] = c.b;
                    break;
                case PixelFormat::RGBA8:
                    m_data[i] = c.r;
                    m_data[i + 1] = c.g;
                    m_data[i + 2] = c.b;
                    m_data[i + 3] = c.a;
                    break;
                case PixelFormat::BGR8:
                    m_data[i] = c.b;
                    m_data[i + 1] = c.g;
                    m_data[i + 2] = c.r;
                    break;
                case PixelFormat::BGRA8:
                    m_data[i] = c.b;
                    m_data[i + 1] = c.g;
                    m_data[i + 2] = c.r;
                    m_data[i + 3] = c.a;
                    break;
                default:
                    break;
                }
            }
        }

        [[nodiscard]] Color32 GetPixel(u32 x, u32 y) const
        {
            if (x >= m_width || y >= m_height)
                return Color32::Black;
            usize off = PixelOffset(x, y);
            switch (m_format)
            {
            case PixelFormat::R8:
            {
                u8 g = m_data[off];
                return {g, g, g, 255};
            }
            case PixelFormat::RGB8:
                return {m_data[off], m_data[off + 1], m_data[off + 2], 255};
            case PixelFormat::RGBA8:
                return {m_data[off], m_data[off + 1], m_data[off + 2], m_data[off + 3]};
            case PixelFormat::BGR8:
                return {m_data[off + 2], m_data[off + 1], m_data[off], 255};
            case PixelFormat::BGRA8:
                return {m_data[off + 2], m_data[off + 1], m_data[off], m_data[off + 3]};
            default:
                return Color32::Black;
            }
        }

        void SetPixel(u32 x, u32 y, Color32 c)
        {
            if (x >= m_width || y >= m_height)
                return;
            usize off = PixelOffset(x, y);
            switch (m_format)
            {
            case PixelFormat::R8:
                m_data[off] = static_cast<u8>((c.r + c.g + c.b) / 3);
                break;
            case PixelFormat::RGB8:
                m_data[off] = c.r;
                m_data[off + 1] = c.g;
                m_data[off + 2] = c.b;
                break;
            case PixelFormat::RGBA8:
                m_data[off] = c.r;
                m_data[off + 1] = c.g;
                m_data[off + 2] = c.b;
                m_data[off + 3] = c.a;
                break;
            case PixelFormat::BGR8:
                m_data[off] = c.b;
                m_data[off + 1] = c.g;
                m_data[off + 2] = c.r;
                break;
            case PixelFormat::BGRA8:
                m_data[off] = c.b;
                m_data[off + 1] = c.g;
                m_data[off + 2] = c.r;
                m_data[off + 3] = c.a;
                break;
            default:
                break;
            }
        }

        // ---- Flips ----

        void FlipVertical()
        {
            u32 rowSize = m_width * BytesPerPixel(m_format);
            Array<u8> tmp(rowSize);
            for (u32 y = 0; y < m_height / 2; ++y)
            {
                u8* top = m_data.Data() + y * rowSize;
                u8* bot = m_data.Data() + (m_height - 1 - y) * rowSize;
                std::memcpy(tmp.Data(), top, rowSize);
                std::memcpy(top, bot, rowSize);
                std::memcpy(bot, tmp.Data(), rowSize);
            }
        }

        void FlipHorizontal()
        {
            u32 bpp = BytesPerPixel(m_format);
            Array<u8> tmp(bpp);
            for (u32 y = 0; y < m_height; ++y)
            {
                for (u32 x = 0; x < m_width / 2; ++x)
                {
                    u8* left = m_data.Data() + PixelOffset(x, y);
                    u8* right = m_data.Data() + PixelOffset(m_width - 1 - x, y);
                    std::memcpy(tmp.Data(), left, bpp);
                    std::memcpy(left, right, bpp);
                    std::memcpy(right, tmp.Data(), bpp);
                }
            }
        }

        // ---- Format conversion ----

        [[nodiscard]] Image ConvertFormat(PixelFormat newFmt) const
        {
            if (newFmt == m_format)
                return *this;
            Image out(m_width, m_height, newFmt);
            for (u32 y = 0; y < m_height; ++y)
                for (u32 x = 0; x < m_width; ++x)
                    out.SetPixel(x, y, GetPixel(x, y));
            return out;
        }

        // ---- Factories ----

        static Image CreateSolidColor(u32 w, u32 h, Color32 c, PixelFormat fmt = PixelFormat::RGBA8)
        {
            Image img(w, h, fmt);
            img.FillColor(c);
            return img;
        }

        static Image CreateCheckerboard(u32 size = 256, Color32 c1 = Color32::White,
                                        Color32 c2 = Color32::Black, u32 checkSize = 32,
                                        PixelFormat fmt = PixelFormat::RGBA8)
        {
            Image img(size, size, fmt);
            for (u32 y = 0; y < size; ++y)
                for (u32 x = 0; x < size; ++x)
                    img.SetPixel(x, y, ((x / checkSize + y / checkSize) % 2 == 0) ? c1 : c2);
            return img;
        }

        static Image CreateGradient(u32 w, u32 h, Color32 top, Color32 bottom,
                                    PixelFormat fmt = PixelFormat::RGBA8)
        {
            Image img(w, h, fmt);
            for (u32 y = 0; y < h; ++y)
            {
                f32 t = static_cast<f32>(y) / static_cast<f32>(h > 1 ? h - 1 : 1);
                Color32 c{
                    static_cast<u8>(top.r + t * (bottom.r - top.r)),
                    static_cast<u8>(top.g + t * (bottom.g - top.g)),
                    static_cast<u8>(top.b + t * (bottom.b - top.b)),
                    static_cast<u8>(top.a + t * (bottom.a - top.a)),
                };
                for (u32 x = 0; x < w; ++x)
                    img.SetPixel(x, y, c);
            }
            return img;
        }

        // ---- Normal-map factories ----
        // Ported from Sedulous.Images.Image. Normals are encoded into RGB as
        // (n*0.5+0.5)*255; the neutral up-normal (0,0,1) -> (128,128,255).

        static Image CreateFlatNormalMap(u32 width = 256, u32 height = 256,
                                         PixelFormat format = PixelFormat::RGBA8)
        {
            Image image(width, height, format);
            image.FillColor(Color32(128, 128, 255, 255)); // (0,0,1)
            return image;
        }

        static Image CreateWaveNormalMap(u32 width = 256, u32 height = 256,
                                         f32 waveFrequencyX = 8.0f, f32 waveFrequencyY = 6.0f,
                                         f32 amplitude = 0.3f,
                                         PixelFormat format = PixelFormat::RGBA8)
        {
            Image image(width, height, format);
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const f32 fx = static_cast<f32>(x) / static_cast<f32>(width);
                    const f32 fy = static_cast<f32>(y) / static_cast<f32>(height);
                    const f32 heightValue = Sin(fx * kPi * waveFrequencyX) * amplitude +
                                            Sin(fy * kPi * waveFrequencyY) * amplitude * 0.7f;
                    const f32 heightRight =
                        Sin((fx + 1.0f / static_cast<f32>(width)) * kPi * waveFrequencyX) *
                            amplitude +
                        Sin(fy * kPi * waveFrequencyY) * amplitude * 0.7f;
                    const f32 heightDown =
                        Sin(fx * kPi * waveFrequencyX) * amplitude +
                        Sin((fy + 1.0f / static_cast<f32>(height)) * kPi * waveFrequencyY) *
                            amplitude * 0.7f;
                    const f32 dx = heightRight - heightValue;
                    const f32 dy = heightDown - heightValue;
                    image.SetPixel(
                        x, y, EncodeNormal(Normalized(Float3{-dx * 20.0f, -dy * 20.0f, 1.0f})));
                }
            }
            return image;
        }

        static Image CreateBrickNormalMap(u32 width = 256, u32 height = 256, u32 bricksX = 8,
                                          u32 bricksY = 4, f32 mortarDepth = 0.3f,
                                          PixelFormat format = PixelFormat::RGBA8)
        {
            Image image(width, height, format);
            const u32 brickWidth = width / bricksX;
            const u32 brickHeight = height / bricksY;
            const u32 mortarWidth = Max(brickWidth / 16u, 2u);

            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const u32 brickY = y / brickHeight;
                    u32 adjustedX = x;
                    if (brickY % 2 == 1)
                        adjustedX = (x + brickWidth / 2) % width;

                    const u32 localX = adjustedX % brickWidth;
                    const u32 localY = y % brickHeight;

                    const bool isHorizontalMortar =
                        localY < mortarWidth || localY >= (brickHeight - mortarWidth);
                    const bool isVerticalMortar =
                        localX < mortarWidth || localX >= (brickWidth - mortarWidth);
                    const bool isMortar = isHorizontalMortar || isVerticalMortar;

                    Float3 normal;
                    if (isMortar)
                    {
                        normal = Float3{0.0f, 0.0f, 1.0f - mortarDepth * 2.0f};
                    }
                    else
                    {
                        const f32 brickVariation = Sin(static_cast<f32>(localX) * 0.2f) *
                                                   Sin(static_cast<f32>(localY) * 0.15f) * 0.1f;
                        normal = Float3{0.0f, 0.0f, 1.0f + brickVariation};
                    }
                    image.SetPixel(x, y, EncodeNormal(Normalized(normal)));
                }
            }
            return image;
        }

        static Image CreateCircularBumpNormalMap(u32 width = 256, u32 height = 256,
                                                 f32 bumpHeight = 0.5f, f32 falloff = 2.0f,
                                                 PixelFormat format = PixelFormat::RGBA8)
        {
            Image image(width, height, format);
            const f32 centerX = static_cast<f32>(width) * 0.5f;
            const f32 centerY = static_cast<f32>(height) * 0.5f;
            const f32 maxRadius = static_cast<f32>(Min(width, height)) * 0.4f;

            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - centerX;
                    const f32 dy = static_cast<f32>(y) - centerY;
                    const f32 distance = Sqrt(dx * dx + dy * dy);

                    Float3 normal;
                    if (distance < maxRadius && distance > 0.001f)
                    {
                        const f32 normalizedDist = distance / maxRadius;
                        const f32 heightDerivative = -falloff *
                                                     Pow(1.0f - normalizedDist, falloff - 1.0f) *
                                                     bumpHeight * 3.0f / maxRadius;
                        const f32 nx = (dx / distance) * heightDerivative;
                        const f32 ny = (dy / distance) * heightDerivative;
                        normal = Normalized(Float3{nx, ny, 1.0f});
                    }
                    else
                    {
                        normal = Float3{0.0f, 0.0f, 1.0f};
                    }
                    image.SetPixel(x, y, EncodeNormal(normal));
                }
            }
            return image;
        }

        static Image CreateNoiseNormalMap(u32 width = 256, u32 height = 256, f32 scale = 0.1f,
                                          f32 amplitude = 0.2f, i32 seed = 12345,
                                          PixelFormat format = PixelFormat::RGBA8)
        {
            Image image(width, height, format);

            Array<f32> heightMap;
            heightMap.Resize(static_cast<usize>(width) * height);

            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    f32 noise = 0.0f;
                    f32 freq = scale;
                    f32 amp = amplitude;
                    for (i32 octave = 0; octave < 4; ++octave)
                    {
                        const f32 fx = static_cast<f32>(x) * freq;
                        const f32 fy = static_cast<f32>(y) * freq;
                        const u32 ix = static_cast<u32>(fx);
                        const u32 iy = static_cast<u32>(fy);
                        const f32 fracX = fx - static_cast<f32>(ix);
                        const f32 fracY = fy - static_cast<f32>(iy);

                        const f32 a = HashToFloat(seed + static_cast<i32>(ix) +
                                                  static_cast<i32>(iy) * 1000 + octave * 10000);
                        const f32 b = HashToFloat(seed + static_cast<i32>(ix + 1) +
                                                  static_cast<i32>(iy) * 1000 + octave * 10000);
                        const f32 c = HashToFloat(seed + static_cast<i32>(ix) +
                                                  static_cast<i32>(iy + 1) * 1000 + octave * 10000);
                        const f32 d = HashToFloat(seed + static_cast<i32>(ix + 1) +
                                                  static_cast<i32>(iy + 1) * 1000 + octave * 10000);

                        const f32 smoothX = fracX * fracX * (3.0f - 2.0f * fracX);
                        const f32 smoothY = fracY * fracY * (3.0f - 2.0f * fracY);

                        const f32 i1 = Lerp(a, b, smoothX);
                        const f32 i2 = Lerp(c, d, smoothX);
                        const f32 value = Lerp(i1, i2, smoothY);

                        noise += value * amp;
                        freq *= 2.0f;
                        amp *= 0.5f;
                    }
                    heightMap[static_cast<usize>(y) * width + x] = noise;
                }
            }

            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    // Edge-clamped neighbours (avoids the u32 underflow latent in the original).
                    const u32 xl = (x > 0) ? x - 1 : 0;
                    const u32 xr = (x + 1 < width) ? x + 1 : width - 1;
                    const u32 yu = (y > 0) ? y - 1 : 0;
                    const u32 yd = (y + 1 < height) ? y + 1 : height - 1;
                    const f32 heightL = heightMap[static_cast<usize>(y) * width + xl];
                    const f32 heightR = heightMap[static_cast<usize>(y) * width + xr];
                    const f32 heightU = heightMap[static_cast<usize>(yu) * width + x];
                    const f32 heightD = heightMap[static_cast<usize>(yd) * width + x];

                    const f32 dx = heightR - heightL;
                    const f32 dy = heightD - heightU;
                    image.SetPixel(x, y,
                                   EncodeNormal(Normalized(Float3{-dx * 8.0f, -dy * 8.0f, 1.0f})));
                }
            }
            return image;
        }

        static Image CreateTestPatternNormalMap(u32 width = 256, u32 height = 256,
                                                PixelFormat format = PixelFormat::RGBA8)
        {
            Image image(width, height, format);
            for (u32 y = 0; y < height; ++y)
            {
                for (u32 x = 0; x < width; ++x)
                {
                    const f32 fx = static_cast<f32>(x) / static_cast<f32>(width);
                    const f32 fy = static_cast<f32>(y) / static_cast<f32>(height);

                    Float3 normal;
                    if (fx < 0.5f && fy < 0.5f)
                    {
                        normal = Float3{0.0f, 0.0f, 1.0f}; // Top-left: flat.
                    }
                    else if (fx >= 0.5f && fy < 0.5f)
                    {
                        const f32 bump = Sin(fx * kPi * 16.0f) * 0.5f; // Top-right: X bumps.
                        normal = Normalized(Float3{bump, 0.0f, 1.0f});
                    }
                    else if (fx < 0.5f && fy >= 0.5f)
                    {
                        const f32 bump = Sin(fy * kPi * 16.0f) * 0.5f; // Bottom-left: Y bumps.
                        normal = Normalized(Float3{0.0f, bump, 1.0f});
                    }
                    else
                    {
                        const f32 centerX = 0.75f, centerY = 0.75f; // Bottom-right: circular.
                        const f32 dx = fx - centerX;
                        const f32 dy = fy - centerY;
                        const f32 dist = Sqrt(dx * dx + dy * dy);
                        if (dist < 0.2f)
                        {
                            const f32 angle = Atan2(dy, dx);
                            normal = Normalized(Float3{Cos(angle) * 0.3f, Sin(angle) * 0.3f, 1.0f});
                        }
                        else
                        {
                            normal = Float3{0.0f, 0.0f, 1.0f};
                        }
                    }
                    image.SetPixel(x, y, EncodeNormal(normal));
                }
            }
            return image;
        }

        /// Build a normal from neighbouring heights (heightmap -> normal conversion).
        [[nodiscard]] static Float3 CalculateNormalFromHeight(f32 heightL, f32 heightR, f32 heightU,
                                                              f32 heightD, f32 scale = 1.0f)
        {
            const f32 dx = (heightR - heightL) * scale;
            const f32 dy = (heightD - heightU) * scale;
            return Normalized(Float3{-dx, -dy, 1.0f});
        }

    private:
        /// Encode a unit normal into a packed RGB color ((n*0.5+0.5)*255), alpha 255.
        [[nodiscard]] static Color32 EncodeNormal(Float3 n)
        {
            return Color32(static_cast<u8>((n.x * 0.5f + 0.5f) * 255.0f),
                           static_cast<u8>((n.y * 0.5f + 0.5f) * 255.0f),
                           static_cast<u8>((n.z * 0.5f + 0.5f) * 255.0f), 255);
        }

        /// LCG-style integer hash -> f32 in [-1, 1] (deterministic; for value noise).
        [[nodiscard]] static f32 HashToFloat(i32 value)
        {
            u32 hash = static_cast<u32>(value);
            hash = hash * 1103515245u + 12345u;
            hash = (hash >> 16) ^ hash;
            hash = hash * 0x85ebca6bu;
            hash = (hash >> 13) ^ hash;
            return (static_cast<f32>(hash & 0x7FFFFFFFu) / static_cast<f32>(0x7FFFFFFF)) * 2.0f -
                   1.0f;
        }

        [[nodiscard]] usize PixelOffset(u32 x, u32 y) const
        {
            return static_cast<usize>(y * m_width + x) * BytesPerPixel(m_format);
        }

        u32 m_width = 0, m_height = 0;
        PixelFormat m_format = PixelFormat::RGBA8;
        ImageColorSpace m_colorSpace = ImageColorSpace::Srgb;
        Array<u8> m_data;
    };

} // namespace draconic::image
