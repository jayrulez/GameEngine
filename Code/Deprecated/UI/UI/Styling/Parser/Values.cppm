// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_value_parser partition
//
// Parses style value literals from .sss token text (shared between the .sss parser and the .sml
// markup loader). Ported from Sedulous.UI/src/Styling/Parser/Values.bf (StyleValueParser).
//
// Divergences (language): Beef Result<Color> -> Optional<Color>; the Beef Color int ctor (0..255)
// is expressed as float components divided by 255 (our Color is float 0..1). Named colors
// missing from core::Color (yellow/cyan/magenta/gray, and the CSS dark "green") are built as float
// literals to match Sedulous exactly.

module;
#include "Core/Prelude.h"

export module foundation.ui:style_value_parser;

import foundation.core; // Color, StringView, Optional
import :thickness;
import :unit;

using namespace foundation::core;

export namespace foundation::ui
{
    struct StyleValueParser
    {
        /// Parse a hex color string: #rrggbb or #rrggbbaa (with leading #).
        [[nodiscard]] static Optional<Color> ParseHexColor(StringView text)
        {
            if (text.Size() < 2 || text[0] != u8'#')
            {
                return {};
            }

            const StringView hex = text.SubStr(1, text.Size() - 1);
            if (hex.Size() == 6)
            {
                Optional<u8> r = ParseHexByte(hex, 0);
                Optional<u8> g = ParseHexByte(hex, 2);
                Optional<u8> b = ParseHexByte(hex, 4);
                if (r.HasValue() && g.HasValue() && b.HasValue())
                    return Color{r.Value() / 255.0f, g.Value() / 255.0f, b.Value() / 255.0f, 1.0f};
            }
            else if (hex.Size() == 8)
            {
                Optional<u8> r = ParseHexByte(hex, 0);
                Optional<u8> g = ParseHexByte(hex, 2);
                Optional<u8> b = ParseHexByte(hex, 4);
                Optional<u8> a = ParseHexByte(hex, 6);
                if (r.HasValue() && g.HasValue() && b.HasValue() && a.HasValue())
                    return Color{r.Value() / 255.0f, g.Value() / 255.0f, b.Value() / 255.0f,
                                 a.Value() / 255.0f};
            }
            return {};
        }

        /// Parse a named color.
        [[nodiscard]] static Optional<Color> ParseNamedColor(StringView name)
        {
            if (name == StringView(u8"white"))
                return Color::White;
            if (name == StringView(u8"black"))
                return Color::Black;
            if (name == StringView(u8"transparent"))
                return Color::Transparent;
            if (name == StringView(u8"red"))
                return Color::Red;
            if (name == StringView(u8"green"))
                return Color{0.0f, 128.0f / 255.0f, 0.0f, 1.0f};
            if (name == StringView(u8"blue"))
                return Color::Blue;
            if (name == StringView(u8"yellow"))
                return Color{1.0f, 1.0f, 0.0f, 1.0f};
            if (name == StringView(u8"cyan"))
                return Color{0.0f, 1.0f, 1.0f, 1.0f};
            if (name == StringView(u8"magenta"))
                return Color{1.0f, 0.0f, 1.0f, 1.0f};
            if (name == StringView(u8"gray"))
                return Color{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f};
            if (name == StringView(u8"grey"))
                return Color{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f};
            return {};
        }

        /// Parse a thickness from 1, 2, or 4 float values.
        /// 1 value: all sides. 2 values: vertical, horizontal. 4 values: top, right, bottom, left.
        [[nodiscard]] static Thickness ParseThickness(const f32* values, i32 count)
        {
            if (count == 1)
                return Thickness{values[0]};
            if (count == 2)
                return Thickness{values[1], values[0], values[1], values[0]}; // horiz, vert
            if (count == 4)
                return Thickness{values[3], values[0], values[1],
                                 values[2]}; // left=3, top=0, right=1, bottom=2
            return Thickness{};
        }

        /// Parse a unit value from a number token.
        /// Unitless = dp (default), "px" = Px, "dp" = Dp, "pt" = Pt, "em" = Em, "%" = Percent.
        [[nodiscard]] static Unit ParseUnit(f32 value, StringView suffix)
        {
            if (suffix == StringView(u8"px"))
                return Unit::Px(value);
            if (suffix == StringView(u8"pt"))
                return Unit::Pt(value);
            if (suffix == StringView(u8"em"))
                return Unit::Em(value);
            if (suffix == StringView(u8"%"))
                return Unit::Percent(value);
            return Unit::Dp(value); // default: dp (includes "dp" and unitless)
        }

        /// Parse a length from TEXT (markup attributes): `240`, `240px`, `16dp`, `12pt`, `50%`,
        /// `2em`, or `calc(a + b)` / `calc(a - b)` over two such terms (one nesting level).
        /// Empty on anything else.
        [[nodiscard]] static Optional<Unit> ParseLengthText(StringView text)
        {
            const StringView t = Trimmed(text);
            const auto startsWith = [](StringView v, StringView prefix)
            { return v.Size() >= prefix.Size() && v.SubStr(0, prefix.Size()) == prefix; };
            const auto endsWith = [](StringView v, StringView suffix)
            {
                return v.Size() >= suffix.Size() &&
                       v.SubStr(v.Size() - suffix.Size(), suffix.Size()) == suffix;
            };
            if (startsWith(t, u8"calc(") && endsWith(t, u8")"))
            {
                const StringView inner = Trimmed(t.SubStr(5, t.Size() - 6));
                // Find the operator: a `+` or `-` surrounded by spaces (a leading sign is not one).
                for (usize i = 1; i + 1 < inner.Size(); ++i)
                {
                    if ((inner[i] == u8'+' || inner[i] == u8'-') && inner[i - 1] == u8' ' &&
                        inner[i + 1] == u8' ')
                    {
                        const Optional<Unit> a = ParseLengthText(inner.SubStr(0, i));
                        const Optional<Unit> b =
                            ParseLengthText(inner.SubStr(i + 1, inner.Size() - i - 1));
                        if (!a.HasValue() || !b.HasValue())
                        {
                            return {};
                        }
                        return inner[i] == u8'+' ? a.Value() + b.Value() : a.Value() - b.Value();
                    }
                }
                return {};
            }
            // Split the trailing unit off the number.
            usize unitStart = t.Size();
            while (unitStart > 0)
            {
                const char8_t c = t[unitStart - 1];
                const bool numeric = (c >= u8'0' && c <= u8'9') || c == u8'.' || c == u8'-';
                if (numeric)
                {
                    break;
                }
                --unitStart;
            }
            const StringView number = t.SubStr(0, unitStart);
            const StringView suffix = t.SubStr(unitStart, t.Size() - unitStart);
            if (number.Size() == 0)
            {
                return {};
            }
            const Optional<f64> v = ParseFloat(number);
            if (!v.HasValue())
            {
                return {};
            }
            if (suffix.Size() != 0 && suffix != u8"px" && suffix != u8"dp" && suffix != u8"pt" &&
                suffix != u8"em" && suffix != u8"%")
            {
                return {};
            }
            return ParseUnit(static_cast<f32>(v.Value()), suffix);
        }

    private:
        [[nodiscard]] static Optional<u8> ParseHexByte(StringView hex, i32 offset)
        {
            if (static_cast<usize>(offset + 2) > hex.Size())
            {
                return {};
            }
            i32 val = 0;
            for (i32 i = 0; i < 2; ++i)
            {
                const utf8char ch = hex[static_cast<usize>(offset + i)];
                val <<= 4;
                if (ch >= u8'0' && ch <= u8'9')
                    val |= ch - u8'0';
                else if (ch >= u8'a' && ch <= u8'f')
                    val |= ch - u8'a' + 10;
                else if (ch >= u8'A' && ch <= u8'F')
                    val |= ch - u8'A' + 10;
                else
                    return {};
            }
            return static_cast<u8>(val);
        }
    };
}
