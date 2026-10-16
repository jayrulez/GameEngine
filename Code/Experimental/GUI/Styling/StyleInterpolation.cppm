// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :style_interpolation partition
//
// Interpolating typed style values (G3 of Specs/vg-and-gui-lab.md), so every property a sheet
// sets can transition and be keyframed, not only opacity and colour: numbers, lengths of one
// unit, colours (straight RGBA), shadow lists (the shorter padded with transparent shadows, as
// CSS does), transform lists of the same functions (function by function), transform origins, and
// gradient layers of the same shape (angle, stop colours and positions). What cannot interpolate
// (keywords, lengths in different units, lists of different functions) switches at the halfway
// point: CSS's discrete animation.

module;
#include "Core/Prelude.h"

export module experimental.gui:style_interpolation;

import foundation.core;
import :css_values;
import :style_value;
import :style_properties;

using namespace foundation::core;
namespace core = foundation::core;

export namespace experimental::gui
{
    namespace detail
    {
        [[nodiscard]] inline f32 Mix(f32 a, f32 b, f32 t) noexcept { return a + (b - a) * t; }

        [[nodiscard]] inline Color MixColor(Color a, Color b, f32 t) noexcept
        {
            return Color{Mix(a.r, b.r, t), Mix(a.g, b.g, t), Mix(a.b, b.b, t), Mix(a.a, b.a, t)};
        }

        [[nodiscard]] inline bool MixLength(LengthValue a, LengthValue b, f32 t, LengthValue& out) noexcept
        {
            // A zero converts to any unit (CSS writes 0 unitless).
            if (a.Value == 0.0f)
                a.Unit = b.Unit;
            if (b.Value == 0.0f)
                b.Unit = a.Unit;
            if (a.Unit != b.Unit)
                return false;
            out = LengthValue{Mix(a.Value, b.Value, t), a.Unit};
            return true;
        }

        [[nodiscard]] inline bool MixShadows(const Array<ShadowValue>& a, const Array<ShadowValue>& b, f32 t,
                                             Array<ShadowValue>& out)
        {
            const usize n = Max(a.Size(), b.Size());
            out.Clear();
            for (usize i = 0; i < n; ++i)
            {
                // The shorter list is padded with a transparent shadow of the other's insetness.
                ShadowValue from = i < a.Size() ? a[i] : ShadowValue{};
                ShadowValue to = i < b.Size() ? b[i] : ShadowValue{};
                if (i >= a.Size())
                {
                    from.Inset = to.Inset;
                    from.ShadowColor = Color{to.ShadowColor.r, to.ShadowColor.g, to.ShadowColor.b, 0.0f};
                }
                if (i >= b.Size())
                {
                    to.Inset = from.Inset;
                    to.ShadowColor = Color{from.ShadowColor.r, from.ShadowColor.g, from.ShadowColor.b, 0.0f};
                }
                if (from.Inset != to.Inset)
                    return false;
                ShadowValue mixed;
                mixed.Inset = from.Inset;
                if (!MixLength(from.OffsetX, to.OffsetX, t, mixed.OffsetX) ||
                    !MixLength(from.OffsetY, to.OffsetY, t, mixed.OffsetY) ||
                    !MixLength(from.Blur, to.Blur, t, mixed.Blur) || !MixLength(from.Spread, to.Spread, t, mixed.Spread))
                    return false;
                mixed.ShadowColor = MixColor(from.ShadowColor, to.ShadowColor, t);
                out.PushBack(mixed);
            }
            return true;
        }

        [[nodiscard]] inline bool MixTransforms(const Array<TransformFunction>& a, const Array<TransformFunction>& b,
                                                f32 t, Array<TransformFunction>& out)
        {
            // none against a list: the list's identity functions (CSS), so `none -> rotate(90deg)`
            // turns from 0.
            const auto identity = [](const TransformFunction& like)
            {
                TransformFunction id;
                id.Kind = like.Kind;
                id.A = LengthValue{0.0f, like.A.Unit};
                id.B = LengthValue{0.0f, like.B.Unit};
                return id; // angle 0, scale 1, matrix identity by default
            };
            const Array<TransformFunction>* from = &a;
            const Array<TransformFunction>* to = &b;
            Array<TransformFunction> padded;
            if (a.IsEmpty() && !b.IsEmpty())
            {
                for (const TransformFunction& f : b)
                    padded.PushBack(identity(f));
                from = &padded;
            }
            else if (b.IsEmpty() && !a.IsEmpty())
            {
                for (const TransformFunction& f : a)
                    padded.PushBack(identity(f));
                to = &padded;
            }
            if (from->Size() != to->Size())
                return false;
            out.Clear();
            for (usize i = 0; i < from->Size(); ++i)
            {
                const TransformFunction& x = (*from)[i];
                const TransformFunction& y = (*to)[i];
                if (x.Kind != y.Kind)
                    return false;
                TransformFunction m = x;
                if (!MixLength(x.A, y.A, t, m.A) || !MixLength(x.B, y.B, t, m.B))
                    return false;
                m.Angle = Mix(x.Angle, y.Angle, t);
                m.AngleY = Mix(x.AngleY, y.AngleY, t);
                m.ScaleX = Mix(x.ScaleX, y.ScaleX, t);
                m.ScaleY = Mix(x.ScaleY, y.ScaleY, t);
                for (usize k = 0; k < 6; ++k)
                    m.M[k] = Mix(x.M[k], y.M[k], t);
                out.PushBack(m);
            }
            return true;
        }

        [[nodiscard]] inline bool MixLayers(const Array<BackgroundLayer>& a, const Array<BackgroundLayer>& b, f32 t,
                                            Array<BackgroundLayer>& out)
        {
            if (a.Size() != b.Size())
                return false;
            out.Clear();
            for (usize i = 0; i < a.Size(); ++i)
            {
                const BackgroundLayer& x = a[i];
                const BackgroundLayer& y = b[i];
                if (x.Kind != y.Kind)
                    return false;
                BackgroundLayer m = x;
                if (x.Kind == BackgroundKind::Gradient)
                {
                    const Array<GradientStopValue> sx = SettleStops(x.Gradient.Stops);
                    const Array<GradientStopValue> sy = SettleStops(y.Gradient.Stops);
                    if (x.Gradient.Kind != y.Gradient.Kind || x.Gradient.Repeating != y.Gradient.Repeating ||
                        sx.Size() != sy.Size())
                        return false;
                    m.Gradient.Angle = Mix(x.Gradient.Angle, y.Gradient.Angle, t);
                    m.Gradient.Stops.Clear();
                    for (usize k = 0; k < sx.Size(); ++k)
                        m.Gradient.Stops.PushBack(GradientStopValue{MixColor(sx[k].StopColor, sy[k].StopColor, t),
                                                                    Mix(sx[k].Position, sy[k].Position, t)});
                }
                else if (x.Kind == BackgroundKind::Color)
                {
                    m.LayerColor = MixColor(x.LayerColor, y.LayerColor, t);
                }
                else if (x.Image != y.Image)
                {
                    return false; // two images do not blend
                }
                out.PushBack(Move(m));
            }
            return true;
        }
    }

    /// `a` toward `b` at `t` (0: a, 1: b; t may leave [0, 1] for an overshooting easing). Values
    /// that cannot interpolate switch at t = 0.5.
    [[nodiscard]] inline StyleValue InterpolateStyleValue(const StyleValue& a, const StyleValue& b, f32 t)
    {
        const auto discrete = [&]() { return t < 0.5f ? a : b; };
        if (a.Kind != b.Kind)
            return discrete();
        StyleValue out = a;
        switch (a.Kind)
        {
        case StyleValueKind::Number:
            out.Number = detail::Mix(a.Number, b.Number, t);
            return out;
        case StyleValueKind::Length:
            return detail::MixLength(a.Length, b.Length, t, out.Length) ? out : discrete();
        case StyleValueKind::ColorValue:
            out.ColorValue = detail::MixColor(a.ColorValue, b.ColorValue, t);
            return out;
        case StyleValueKind::Shadows:
            return detail::MixShadows(a.Shadows, b.Shadows, t, out.Shadows) ? out : discrete();
        case StyleValueKind::Transforms:
            return detail::MixTransforms(a.Transforms, b.Transforms, t, out.Transforms) ? out : discrete();
        case StyleValueKind::Origin:
            if (detail::MixLength(a.OriginX, b.OriginX, t, out.OriginX) &&
                detail::MixLength(a.OriginY, b.OriginY, t, out.OriginY))
                return out;
            return discrete();
        case StyleValueKind::Layers:
            return detail::MixLayers(a.Layers, b.Layers, t, out.Layers) ? out : discrete();
        case StyleValueKind::Keyword:
        case StyleValueKind::None:
            return discrete();
        }
        return discrete();
    }
}
