// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

module;
#include "Core/Prelude.h"
#include "Core/Debug/Assert.h"

export module foundation.core:color;

import :base;
import :math;

export namespace foundation::core
{
    // =======================================================================
    // Color - RGBA, float components (typically 0..1). An authored colour (a component's
    // colour, a material's colour property, a UI colour) is sRGB: the value a colour picker and
    // a hex code show, the same encoding as an sRGB image. The renderer decodes it to linear
    // (ToLinear below) where it hands the colour to the GPU, as the hardware decodes an sRGB
    // texture on sample; render data and shading carry linear values.
    // =======================================================================
    struct ENGINE_EXPORT_DATA Color
    {
        f32 r = 0.0f;
        f32 g = 0.0f;
        f32 b = 0.0f;
        f32 a = 1.0f;

        constexpr Color() noexcept = default;
        constexpr Color(f32 inR, f32 inG, f32 inB, f32 inA = 1.0f) noexcept
            : r(inR), g(inG), b(inB), a(inA)
        {
        }

        // Packs to 0xRRGGBBAA (components clamped to 0..1).
        [[nodiscard]] u32 ToRGBA8() const noexcept
        {
            const auto byteOf = [](f32 c) -> u32
            { return static_cast<u32>(Clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f); };
            return (byteOf(r) << 24) | (byteOf(g) << 16) | (byteOf(b) << 8) | byteOf(a);
        }

        [[nodiscard]] static Color FromRGBA8(u32 packed) noexcept
        {
            return Color{static_cast<f32>((packed >> 24) & 0xFFu) / 255.0f,
                         static_cast<f32>((packed >> 16) & 0xFFu) / 255.0f,
                         static_cast<f32>((packed >> 8) & 0xFFu) / 255.0f,
                         static_cast<f32>(packed & 0xFFu) / 255.0f};
        }

        // Build from 0-255 channel bytes (the common literal form: Color::Rgb(28, 28, 33)). Saves
        // every UI widget rolling its own 0-255 helper.
        [[nodiscard]] static constexpr Color Rgb(u8 r, u8 g, u8 b, u8 a = 255) noexcept
        {
            return Color{static_cast<f32>(r) / 255.0f, static_cast<f32>(g) / 255.0f,
                         static_cast<f32>(b) / 255.0f, static_cast<f32>(a) / 255.0f};
        }

        static const Color White;
        static const Color Black;
        static const Color Red;
        static const Color Green;
        static const Color Blue;
        static const Color Transparent;
    };

    inline constexpr Color Color::White{1.0f, 1.0f, 1.0f, 1.0f};
    inline constexpr Color Color::Black{0.0f, 0.0f, 0.0f, 1.0f};
    inline constexpr Color Color::Red{1.0f, 0.0f, 0.0f, 1.0f};
    inline constexpr Color Color::Green{0.0f, 1.0f, 0.0f, 1.0f};
    inline constexpr Color Color::Blue{0.0f, 0.0f, 1.0f, 1.0f};
    inline constexpr Color Color::Transparent{0.0f, 0.0f, 0.0f, 0.0f};

    [[nodiscard]] constexpr Color operator*(Color c, f32 s) noexcept
    {
        return {c.r * s, c.g * s, c.b * s, c.a * s};
    }
    [[nodiscard]] constexpr Color operator+(Color a, Color b) noexcept
    {
        return {a.r + b.r, a.g + b.g, a.b + b.b, a.a + b.a};
    }
    [[nodiscard]] constexpr bool operator==(Color a, Color b) noexcept
    {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    [[nodiscard]] constexpr Color Lerp(Color a, Color b, f32 t) noexcept
    {
        return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
    }

    [[nodiscard]] inline bool NearlyEqual(Color a, Color b, f32 epsilon = kEpsilon) noexcept
    {
        return NearlyEqual(a.r, b.r, epsilon) && NearlyEqual(a.g, b.g, epsilon) &&
               NearlyEqual(a.b, b.b, epsilon) && NearlyEqual(a.a, b.a, epsilon);
    }

    // =======================================================================
    // Color32 - packed 8-bit RGBA, for compact storage: image pixels, vertex
    // colors, asset interchange. The byte-color counterpart of the float Color.
    // Conversions are plain 0..255 <-> 0..1 with no gamma; sRGB to linear is
    // handled explicitly at the texture and format boundary (ImageColorSpace,
    // *UnormSrgb formats).
    // =======================================================================
    struct ENGINE_EXPORT_DATA Color32
    {
        u8 r = 0;
        u8 g = 0;
        u8 b = 0;
        u8 a = 255;

        constexpr Color32() noexcept = default;
        constexpr Color32(u8 inR, u8 inG, u8 inB, u8 inA = 255) noexcept
            : r(inR), g(inG), b(inB), a(inA)
        {
        }

        // Packs to 0xRRGGBBAA.
        [[nodiscard]] constexpr u32 ToRGBA8() const noexcept
        {
            return (static_cast<u32>(r) << 24) | (static_cast<u32>(g) << 16) |
                   (static_cast<u32>(b) << 8) | static_cast<u32>(a);
        }

        [[nodiscard]] static constexpr Color32 FromRGBA8(u32 packed) noexcept
        {
            return Color32{static_cast<u8>((packed >> 24) & 0xFFu),
                           static_cast<u8>((packed >> 16) & 0xFFu),
                           static_cast<u8>((packed >> 8) & 0xFFu), static_cast<u8>(packed & 0xFFu)};
        }

        static const Color32 White;
        static const Color32 Black;
        static const Color32 Red;
        static const Color32 Green;
        static const Color32 Blue;
        static const Color32 Transparent;
    };

    inline constexpr Color32 Color32::White{255, 255, 255, 255};
    inline constexpr Color32 Color32::Black{0, 0, 0, 255};
    inline constexpr Color32 Color32::Red{255, 0, 0, 255};
    inline constexpr Color32 Color32::Green{0, 255, 0, 255};
    inline constexpr Color32 Color32::Blue{0, 0, 255, 255};
    inline constexpr Color32 Color32::Transparent{0, 0, 0, 0};

    [[nodiscard]] constexpr bool operator==(Color32 a, Color32 b) noexcept
    {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    }

    // Float Color -> packed bytes (components clamped to 0..1, rounded).
    [[nodiscard]] constexpr Color32 ToColor32(Color c) noexcept
    {
        const auto byteOf = [](f32 v) -> u8
        { return static_cast<u8>(Clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return Color32{byteOf(c.r), byteOf(c.g), byteOf(c.b), byteOf(c.a)};
    }

    // Packed bytes -> float Color (exact 0..255 -> 0..1; round-trips ToColor32).
    [[nodiscard]] constexpr Color ToColor(Color32 c) noexcept
    {
        return Color{static_cast<f32>(c.r) / 255.0f, static_cast<f32>(c.g) / 255.0f,
                     static_cast<f32>(c.b) / 255.0f, static_cast<f32>(c.a) / 255.0f};
    }

    // sRGB <-> linear transfer functions (the standard IEC 61966-2-1 EOTF). For
    // decoding sRGB-authored colors to linear before linear-space blending/shading.
    [[nodiscard]] inline f32 SrgbToLinear(f32 c) noexcept
    {
        return (c <= 0.04045f) ? (c / 12.92f) : Pow((c + 0.055f) / 1.055f, 2.4f);
    }

    [[nodiscard]] inline f32 LinearToSrgb(f32 c) noexcept
    {
        return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * Pow(c, 1.0f / 2.4f) - 0.055f);
    }

    // An authored (sRGB) Color -> linear, for render data; alpha is coverage and stays as it is.
    // Components above 1 decode along the same curve, so an over-bright value stays over-bright.
    [[nodiscard]] inline Color ToLinear(Color c) noexcept
    {
        return Color{SrgbToLinear(c.r), SrgbToLinear(c.g), SrgbToLinear(c.b), c.a};
    }

    // Linear -> sRGB, the inverse: for colour values that arrive linear (a glTF factor).
    [[nodiscard]] inline Color ToSrgb(Color c) noexcept
    {
        return Color{LinearToSrgb(c.r), LinearToSrgb(c.g), LinearToSrgb(c.b), c.a};
    }

    // Color32 (sRGB-authored) -> linear float Color (RGB through the sRGB EOTF;
    // alpha is linear). For uploading UI/SVG colors to a linear/sRGB pipeline.
    [[nodiscard]] inline Color ToLinear(Color32 c) noexcept
    {
        return Color{SrgbToLinear(static_cast<f32>(c.r) / 255.0f),
                     SrgbToLinear(static_cast<f32>(c.g) / 255.0f),
                     SrgbToLinear(static_cast<f32>(c.b) / 255.0f), static_cast<f32>(c.a) / 255.0f};
    }
}
