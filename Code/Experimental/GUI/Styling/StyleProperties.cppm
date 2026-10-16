// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_properties partition
//
// The CSS properties the lab knows (G2 of Specs/vg-and-gui-lab.md), as one table instead of a
// chain of string compares: each property's name, how its value parses into a typed StyleValue,
// and the shorthands that expand into longhands (`border` into twelve, `border-radius` into four)
// so the cascade works per longhand, as CSS's does. A property the table does not name may still
// reach a widget: a reflected property that declares its CSS name (the `css` property attribute)
// takes it, parsed by its type, so a library's widget gets CSS for its own properties with no
// applier code (StyleApplier).

module;
#include "Core/Prelude.h"

export module experimental.gui:style_properties;

import foundation.core;
import :css_values;
import :style_value;
import :motion; // the transition and animation shorthands' expansion

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    enum class StyleValueKind : u8
    {
        None,       ///< Not parsed (an unknown property's text, a custom property)
        Number,     ///< opacity
        Length,     ///< width, border-top-width, font-size, a corner radius
        ColorValue, ///< color, background-color, border-top-color
        Keyword,    ///< text-align, visibility, border-top-style, enabled
        Shadows,    ///< box-shadow
        Transforms, ///< transform
        Origin,     ///< transform-origin (two lengths)
        Layers,     ///< background-image
    };

    /// A declaration's value once parsed.
    struct StyleValue
    {
        StyleValueKind Kind = StyleValueKind::None;
        f32 Number = 0.0f;
        LengthValue Length{};
        Color ColorValue{};
        core::String Keyword;
        Array<ShadowValue> Shadows;
        Array<TransformFunction> Transforms;
        LengthValue OriginX{50.0f, LengthUnit::Percent};
        LengthValue OriginY{50.0f, LengthUnit::Percent};
        Array<BackgroundLayer> Layers;
    };

    /// How a property's value parses.
    enum class StyleGrammar : u8
    {
        Number,
        Length,
        Color,
        Keyword,
        Shadows,
        Transforms,
        Origin,
        Layers,
        Text, ///< kept as written (font-family, transition, animation)
    };

    struct StylePropertySpec
    {
        const char8_t* Name;
        StyleGrammar Grammar;
        /// CSS's initial value, for a property applied through a setter that an ended animation
        /// returns to when the sheet sets none (null: the node keeps its own value). The box
        /// properties need none: the box is rebuilt from the style on every apply.
        const char8_t* Initial = nullptr;
    };

    /// The longhands the lab knows, by name.
    [[nodiscard]] inline const StylePropertySpec* FindStyleProperty(core::StringView name)
    {
        static constexpr StylePropertySpec kProperties[] = {
            {u8"opacity", StyleGrammar::Number, u8"1"},
            {u8"width", StyleGrammar::Length},
            {u8"height", StyleGrammar::Length},
            {u8"min-width", StyleGrammar::Length},
            {u8"min-height", StyleGrammar::Length},
            {u8"max-width", StyleGrammar::Length},
            {u8"max-height", StyleGrammar::Length},
            {u8"padding-top", StyleGrammar::Length, u8"0"},
            {u8"padding-right", StyleGrammar::Length, u8"0"},
            {u8"padding-bottom", StyleGrammar::Length, u8"0"},
            {u8"padding-left", StyleGrammar::Length, u8"0"},
            {u8"margin-top", StyleGrammar::Length, u8"0"},
            {u8"margin-right", StyleGrammar::Length, u8"0"},
            {u8"margin-bottom", StyleGrammar::Length, u8"0"},
            {u8"margin-left", StyleGrammar::Length, u8"0"},
            {u8"enabled", StyleGrammar::Keyword},
            {u8"visibility", StyleGrammar::Keyword, u8"visible"},
            {u8"color", StyleGrammar::Color},
            {u8"text-align", StyleGrammar::Keyword},
            {u8"vertical-align", StyleGrammar::Keyword},
            {u8"font-family", StyleGrammar::Text},
            {u8"font-size", StyleGrammar::Length},
            {u8"background-color", StyleGrammar::Color},
            {u8"background-image", StyleGrammar::Layers},
            {u8"border-top-width", StyleGrammar::Length},
            {u8"border-right-width", StyleGrammar::Length},
            {u8"border-bottom-width", StyleGrammar::Length},
            {u8"border-left-width", StyleGrammar::Length},
            {u8"border-top-color", StyleGrammar::Color},
            {u8"border-right-color", StyleGrammar::Color},
            {u8"border-bottom-color", StyleGrammar::Color},
            {u8"border-left-color", StyleGrammar::Color},
            {u8"border-top-style", StyleGrammar::Keyword},
            {u8"border-right-style", StyleGrammar::Keyword},
            {u8"border-bottom-style", StyleGrammar::Keyword},
            {u8"border-left-style", StyleGrammar::Keyword},
            {u8"border-top-left-radius", StyleGrammar::Length},
            {u8"border-top-right-radius", StyleGrammar::Length},
            {u8"border-bottom-right-radius", StyleGrammar::Length},
            {u8"border-bottom-left-radius", StyleGrammar::Length},
            {u8"box-shadow", StyleGrammar::Shadows},
            {u8"transform", StyleGrammar::Transforms},
            {u8"transform-origin", StyleGrammar::Origin},
            {u8"transition-property", StyleGrammar::Text},
            {u8"transition-duration", StyleGrammar::Text},
            {u8"transition-timing-function", StyleGrammar::Text},
            {u8"transition-delay", StyleGrammar::Text},
            {u8"animation-name", StyleGrammar::Text},
            {u8"animation-duration", StyleGrammar::Text},
            {u8"animation-timing-function", StyleGrammar::Text},
            {u8"animation-delay", StyleGrammar::Text},
            {u8"animation-iteration-count", StyleGrammar::Text},
            {u8"animation-direction", StyleGrammar::Text},
            {u8"animation-fill-mode", StyleGrammar::Text},
            {u8"animation-play-state", StyleGrammar::Text},
        };
        for (const StylePropertySpec& spec : kProperties)
            if (core::StringView(spec.Name) == name)
                return &spec;
        return nullptr;
    }

    /// `text` as a value of `grammar`; false when it does not parse.
    [[nodiscard]] inline bool ParseStyleValue(StyleGrammar grammar, core::StringView text, StyleValue& out)
    {
        const core::StringView v = Trim(text);
        switch (grammar)
        {
        case StyleGrammar::Number:
            if (Optional<f32> n = ParseLength(v); n.HasValue())
            {
                out.Kind = StyleValueKind::Number;
                out.Number = n.Value();
                return true;
            }
            return false;
        case StyleGrammar::Length:
            if (Optional<LengthValue> l = ParseLengthValue(v); l.HasValue())
            {
                out.Kind = StyleValueKind::Length;
                out.Length = l.Value();
                return true;
            }
            return false;
        case StyleGrammar::Color:
            if (Optional<Color> c = ParseColor(v); c.HasValue())
            {
                out.Kind = StyleValueKind::ColorValue;
                out.ColorValue = c.Value();
                return true;
            }
            return false;
        case StyleGrammar::Keyword:
        case StyleGrammar::Text:
            if (v.Size() == 0)
                return false;
            out.Kind = StyleValueKind::Keyword;
            out.Keyword = core::String(v);
            return true;
        case StyleGrammar::Shadows:
            if (Optional<Array<ShadowValue>> s = ParseShadows(v); s.HasValue())
            {
                out.Kind = StyleValueKind::Shadows;
                out.Shadows = Move(s.Value());
                return true;
            }
            return false;
        case StyleGrammar::Transforms:
            if (Optional<Array<TransformFunction>> t = ParseTransforms(v); t.HasValue())
            {
                out.Kind = StyleValueKind::Transforms;
                out.Transforms = Move(t.Value());
                return true;
            }
            return false;
        case StyleGrammar::Origin:
            if (Optional<TransformOriginValue> o = ParseTransformOrigin(v); o.HasValue())
            {
                out.Kind = StyleValueKind::Origin;
                out.OriginX = o.Value().X;
                out.OriginY = o.Value().Y;
                return true;
            }
            return false;
        case StyleGrammar::Layers:
            if (Optional<Array<BackgroundLayer>> l = ParseBackgroundLayers(v); l.HasValue())
            {
                out.Kind = StyleValueKind::Layers;
                out.Layers = Move(l.Value());
                return true;
            }
            return false;
        }
        return false;
    }

    /// One longhand a declaration sets.
    struct StyleLonghand
    {
        core::String Name;
        core::String Text;
    };

    namespace detail
    {
        inline void Push(Array<StyleLonghand>& out, core::StringView name, core::StringView text)
        {
            out.PushBack(StyleLonghand{core::String(name), core::String(text)});
        }

        [[nodiscard]] inline bool IsBorderStyle(core::StringView t)
        {
            return t == core::StringView(u8"solid") || t == core::StringView(u8"none") ||
                   t == core::StringView(u8"hidden") || t == core::StringView(u8"dashed") ||
                   t == core::StringView(u8"dotted") || t == core::StringView(u8"double");
        }

        // `<width> || <style> || <color>`, any order: the three parts, CSS's initial values where
        // unsaid (medium is 3px; the colour, black here).
        [[nodiscard]] inline bool SplitBorder(core::StringView value, core::String& width, core::String& style,
                                              core::String& color)
        {
            width = core::String(u8"3px");
            style = core::String(u8"none");
            color = core::String(u8"#000000");
            bool w = false, s = false, c = false;
            for (const core::StringView token : SplitTopLevel(value, u8' '))
            {
                if (!s && IsBorderStyle(token))
                {
                    style = core::String(token);
                    s = true;
                }
                else if (!w && ParseLengthValue(token).HasValue())
                {
                    width = core::String(token);
                    w = true;
                }
                else if (!c && ParseColor(token).HasValue())
                {
                    color = core::String(token);
                    c = true;
                }
                else
                {
                    return false;
                }
            }
            return w || s || c; // no style: none, so nothing draws (CSS)
        }

        inline constexpr const char8_t* kSides[4] = {u8"top", u8"right", u8"bottom", u8"left"};
        inline constexpr const char8_t* kCorners[4] = {u8"top-left", u8"top-right", u8"bottom-right", u8"bottom-left"};

        [[nodiscard]] inline core::String Joined(const char8_t* a, const char8_t* b, const char8_t* c)
        {
            core::String s(a);
            s.Append(core::StringView(b));
            s.Append(core::StringView(c));
            return s;
        }
    }

    /// The longhands `name: text` sets: itself when it is one, its longhands when it is a
    /// shorthand (border, border-<side>, border-width, border-color, border-style, border-radius,
    /// padding, margin, background, transition, animation). False when a shorthand's value does not parse.
    [[nodiscard]] inline bool ExpandStyleShorthand(core::StringView name, core::StringView text,
                                                   Array<StyleLonghand>& out)
    {
        using detail::Joined;
        using detail::kSides;
        const core::StringView value = Trim(text);
        const auto sides = [&](const char8_t* prefix, const char8_t* suffix)
        {
            const Optional<Array<core::StringView>> parts =
                ParseSides<core::StringView>(value, [](core::StringView t) { return Optional<core::StringView>(t); });
            if (!parts.HasValue())
                return false;
            for (usize k = 0; k < 4; ++k)
                detail::Push(out, Joined(prefix, kSides[k], suffix).AsView(), parts.Value()[k]);
            return true;
        };
        if (name == core::StringView(u8"border"))
        {
            core::String w, s, c;
            if (!detail::SplitBorder(value, w, s, c))
                return false;
            for (const char8_t* side : kSides)
            {
                detail::Push(out, Joined(u8"border-", side, u8"-width").AsView(), w.AsView());
                detail::Push(out, Joined(u8"border-", side, u8"-style").AsView(), s.AsView());
                detail::Push(out, Joined(u8"border-", side, u8"-color").AsView(), c.AsView());
            }
            return true;
        }
        for (const char8_t* side : kSides)
        {
            if (name == Joined(u8"border-", side, u8"").AsView())
            {
                core::String w, s, c;
                if (!detail::SplitBorder(value, w, s, c))
                    return false;
                detail::Push(out, Joined(u8"border-", side, u8"-width").AsView(), w.AsView());
                detail::Push(out, Joined(u8"border-", side, u8"-style").AsView(), s.AsView());
                detail::Push(out, Joined(u8"border-", side, u8"-color").AsView(), c.AsView());
                return true;
            }
        }
        if (name == core::StringView(u8"border-width"))
            return sides(u8"border-", u8"-width");
        if (name == core::StringView(u8"border-color"))
            return sides(u8"border-", u8"-color");
        if (name == core::StringView(u8"border-style"))
            return sides(u8"border-", u8"-style");
        if (name == core::StringView(u8"padding"))
            return sides(u8"padding-", u8"");
        if (name == core::StringView(u8"margin"))
            return sides(u8"margin-", u8"");
        if (name == core::StringView(u8"border-radius"))
        {
            const Optional<Array<core::StringView>> parts =
                ParseSides<core::StringView>(value, [](core::StringView t) { return Optional<core::StringView>(t); });
            if (!parts.HasValue())
                return false;
            for (usize k = 0; k < 4; ++k)
                detail::Push(out, Joined(u8"border-", detail::kCorners[k], u8"-radius").AsView(), parts.Value()[k]);
            return true;
        }
        if (name == core::StringView(u8"transition") || name == core::StringView(u8"animation"))
        {
            Array<core::String> names, values;
            if (name == core::StringView(u8"transition"))
                ExpandTransition(value, names, values);
            else
                ExpandAnimation(value, names, values);
            for (usize i = 0; i < names.Size(); ++i)
                detail::Push(out, names[i].AsView(), values[i].AsView());
            return true;
        }
        if (name == core::StringView(u8"background"))
        {
            // Layers, the bottom one possibly a colour: the colour to background-color, the rest
            // (or none) to background-image.
            const Array<core::StringView> layers = SplitTopLevel(value, u8',');
            if (layers.IsEmpty())
                return false;
            core::StringView color = core::StringView(u8"transparent");
            usize imageCount = layers.Size();
            if (ParseColor(layers[layers.Size() - 1]).HasValue())
            {
                color = layers[layers.Size() - 1];
                --imageCount;
            }
            core::String images;
            for (usize k = 0; k < imageCount; ++k)
            {
                if (k > 0)
                    images.Append(core::StringView(u8", "));
                images.Append(layers[k]);
            }
            detail::Push(out, core::StringView(u8"background-color"), color);
            detail::Push(out, core::StringView(u8"background-image"),
                         imageCount == 0 ? core::StringView(u8"none") : images.AsView());
            return true;
        }
        detail::Push(out, name, value);
        return true;
    }
}
