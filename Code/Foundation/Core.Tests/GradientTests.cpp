// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Gradient (foundation.core:gradient): colour stops and alpha stops apart, blended in sRGB values,
// as light, in OKLab, or held; the ends clamped; empty stops white and opaque.

#include <doctest/doctest.h>

import foundation.core;

using namespace foundation::core;

namespace
{
    Color At(Span<const GradientColorStop> colors, Span<const GradientAlphaStop> alphas, GradientInterpolation how, f32 t)
    {
        return EvaluateGradient(colors, alphas, how, t);
    }
}

TEST_CASE("gradient: no stops is white and opaque; one stop is its colour throughout")
{
    const Color white = At({}, {}, GradientInterpolation::Srgb, 0.5f);
    CHECK(white.r == doctest::Approx(1.0f));
    CHECK(white.a == doctest::Approx(1.0f));
    const GradientColorStop one[] = {{0.3f, Color{0.2f, 0.4f, 0.6f, 0.1f}}};
    const Color c = At(Span<const GradientColorStop>(one, 1), {}, GradientInterpolation::Srgb, 0.9f);
    CHECK(c.b == doctest::Approx(0.6f));
    CHECK(c.a == doctest::Approx(1.0f)); // a colour stop's own alpha is not the gradient's
}

TEST_CASE("gradient: sRGB values blend straight, light blends brighter, OKLab keeps lightness even, constant holds")
{
    const GradientColorStop stops[] = {{0.0f, Color{1.0f, 0.0f, 0.0f, 1.0f}}, {1.0f, Color{0.0f, 0.0f, 1.0f, 1.0f}}};
    const Span<const GradientColorStop> span(stops, 2);
    const Color srgb = At(span, {}, GradientInterpolation::Srgb, 0.5f);
    CHECK(srgb.r == doctest::Approx(0.5f));
    CHECK(srgb.b == doctest::Approx(0.5f));
    const Color light = At(span, {}, GradientInterpolation::LinearLight, 0.5f);
    CHECK(light.r == doctest::Approx(LinearToSrgb(0.5f)).epsilon(0.001)); // half the light, encoded
    CHECK(light.r > srgb.r);
    const Color oklab = At(span, {}, GradientInterpolation::OkLab, 0.5f);
    CHECK(oklab.r > 0.0f);
    CHECK(oklab.b > 0.0f);
    const Color ends = At(span, {}, GradientInterpolation::OkLab, 1.0f);
    CHECK(ends.b == doctest::Approx(1.0f).epsilon(0.001)); // an end is its stop exactly
    CHECK(ends.r == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(At(span, {}, GradientInterpolation::Constant, 0.99f).r == doctest::Approx(1.0f));
    CHECK(At(span, {}, GradientInterpolation::Srgb, -1.0f).r == doctest::Approx(1.0f)); // clamped before
}

TEST_CASE("gradient: alpha stops apart from the colours, straight between them, held under Constant")
{
    const GradientColorStop colors[] = {{0.0f, Color{1.0f, 1.0f, 1.0f, 1.0f}}};
    const GradientAlphaStop alphas[] = {{0.0f, 1.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    const Span<const GradientColorStop> c(colors, 1);
    const Span<const GradientAlphaStop> a(alphas, 3);
    CHECK(At(c, a, GradientInterpolation::Srgb, 0.25f).a == doctest::Approx(1.0f));
    CHECK(At(c, a, GradientInterpolation::Srgb, 0.75f).a == doctest::Approx(0.5f));
    CHECK(At(c, a, GradientInterpolation::Constant, 0.75f).a == doctest::Approx(1.0f));
    CHECK(At(c, a, GradientInterpolation::LinearLight, 2.0f).a == doctest::Approx(0.0f));
}

TEST_CASE("gradient: Core reflects its stops and a curve's keys, so a tool reads them without the runtime holding them")
{
    RegisterCoreTypes();
    CHECK(FindProperty(TypeOf<CurveKey>(), "tangentMode") != nullptr);
    CHECK(FindProperty(TypeOf<GradientColorStop>(), "color") != nullptr);
    CHECK(FindProperty(TypeOf<GradientAlphaStop>(), "alpha") != nullptr);
    CHECK(StringView(reinterpret_cast<const char8_t*>(TypeOf<CurveKey>().namespaceName)) == StringView(u8"rtti::core"));
    CHECK(Enumerators(TypeOf<GradientInterpolation>()).Size() == 4u);
    CHECK(Enumerators(TypeOf<CurveTangentMode>()).Size() == 4u);
    CHECK(GlobalTypeRegistry().FindByName("rtti::core", "CurveKey") == &TypeOf<CurveKey>());
}
