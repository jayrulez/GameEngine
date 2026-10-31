// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Particle curves on Core's types: float curves of CurveKeys sampled by EvaluateCurve, colour
// curves as gradients, size over lifetime as `size` with `separateAxes` and `sizeY`; the layout
// round-tripped through text.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.xml;
import foundation.xml.serialization;
import foundation.particles;

using namespace foundation::core;
namespace particles = foundation::particles;
namespace xml = foundation::xml;

namespace
{
    // Reads `markup` (an object's fields, as the XmlSerializer writes them) into `read`.
    template <typename F>
    bool ReadXml(const char8_t* markup, F&& read)
    {
        xml::XmlDocument doc(DefaultAllocator());
        if (doc.Parse(StringView(markup)) != xml::XmlResult::Ok)
            return false;
        xml::XmlSerializer ar(doc);
        read(ar);
        return ar.IsOk();
    }
    template <typename F>
    String WriteXml(F&& write)
    {
        xml::XmlSerializer ar(DefaultAllocator());
        write(ar);
        String out;
        ar.GetOutput(out);
        return out;
    }
}

TEST_CASE("particle curves: a float curve of Core's keys, cubic by default as particle curves were, linear when asked")
{
    particles::ParticleCurveFloat eased;
    eased.AddKey(0.0f, 0.0f);
    eased.AddKey(1.0f, 10.0f);
    CHECK(eased.keys[0].interpolation == CurveKeyInterpolation::Cubic);
    CHECK(eased.Evaluate(0.25f) == doctest::Approx(10.0f * 0.15625f)); // smoothstep(0.25)
    const particles::ParticleCurveFloat straight = particles::ParticleCurveFloat::Linear(0.0f, 10.0f);
    CHECK(straight.Evaluate(0.25f) == doctest::Approx(2.5f));
    CHECK(particles::ParticleCurveFloat::Ease(0.0f, 10.0f).Evaluate(0.5f) == doctest::Approx(5.0f));
    CHECK(particles::ParticleCurveFloat::Ease(0.0f, 10.0f).Evaluate(0.25f) == doctest::Approx(eased.Evaluate(0.25f)));
    // The same function Core's curve samples with.
    CHECK(straight.Evaluate(0.7f) == doctest::Approx(EvaluateCurve(Span<const CurveKey>(straight.keys, 2), 0.7f)));
}

TEST_CASE("particle curves: a colour curve is a gradient, its alpha stops apart")
{
    const particles::ParticleCurveColor fade = particles::ParticleCurveColor::FadeAlpha(Float4{1.0f, 0.5f, 0.0f, 1.0f}, 0.5f);
    CHECK(fade.colorCount == 1);
    CHECK(fade.alphaCount == 3);
    CHECK(fade.Evaluate(0.25f).w == doctest::Approx(1.0f));
    CHECK(fade.Evaluate(0.75f).w == doctest::Approx(0.5f));
    CHECK(fade.Evaluate(0.75f).y == doctest::Approx(0.5f));
    particles::ParticleCurveColor ramp;
    ramp.AddKey(0.0f, Float4{0.0f, 0.0f, 0.0f, 1.0f});
    ramp.AddKey(1.0f, Float4{1.0f, 1.0f, 1.0f, 0.0f});
    CHECK(ramp.Evaluate(0.5f).x == doctest::Approx(0.5f)); // sRGB values, straight
    CHECK(ramp.Evaluate(0.5f).w == doctest::Approx(0.5f));
    ramp.interpolation = GradientInterpolation::LinearLight;
    CHECK(ramp.Evaluate(0.5f).x > 0.5f);
    CHECK(particles::ParticleCurveColor{}.Evaluate(0.3f).x == doctest::Approx(1.0f)); // empty: white
}

TEST_CASE("particle curves: size over lifetime sets both axes from `size`, or Y from `sizeY` with separate axes")
{
    particles::ParticleStreamContainer streams(8);
    particles::SizeOverLifetimeBehavior behavior;
    behavior.size = particles::ParticleCurveFloat::Linear(1.0f, 3.0f);
    behavior.sizeY = particles::ParticleCurveFloat::Constant(9.0f);
    behavior.DeclareStreams(streams);
    streams.aliveCount = 1;
    (*streams.Ages())[0] = 0.5f;
    (*streams.Lifetimes())[0] = 1.0f;
    Random rng(1);
    particles::ParticleUpdateContext ctx{0.0f, 0.016f, Float3::Zero, &rng};
    behavior.Update(streams, ctx);
    CHECK((*streams.Sizes())[0].x == doctest::Approx(2.0f));
    CHECK((*streams.Sizes())[0].y == doctest::Approx(2.0f)); // one curve, both axes
    behavior.separateAxes = true;
    behavior.Update(streams, ctx);
    CHECK((*streams.Sizes())[0].y == doctest::Approx(9.0f));
}

TEST_CASE("particle curves: the new layout round-trips through text")
{
    particles::SizeOverLifetimeBehavior size;
    size.size = particles::ParticleCurveFloat::Ease(0.5f, 2.0f);
    size.separateAxes = true;
    size.sizeY = particles::ParticleCurveFloat::Linear(1.0f, 4.0f);
    particles::ColorOverLifetimeBehavior color;
    color.curve = particles::ParticleCurveColor::FadeAlpha(Float4{0.2f, 0.4f, 0.6f, 1.0f});
    color.curve.interpolation = GradientInterpolation::OkLab;
    const String sizeText = WriteXml([&](ISerializer& ar) { size.Serialize(ar); });
    const String colorText = WriteXml([&](ISerializer& ar) { color.Serialize(ar); });

    particles::SizeOverLifetimeBehavior sizeBack;
    REQUIRE(ReadXml(reinterpret_cast<const char8_t*>(sizeText.Data()), [&](ISerializer& ar) { sizeBack.Serialize(ar); }));
    CHECK(sizeBack.separateAxes);
    CHECK(sizeBack.size.keyCount == 2);
    CHECK(sizeBack.size.keys[1].tangentMode == CurveTangentMode::Flat);
    CHECK(sizeBack.sizeY.keys[1].interpolation == CurveKeyInterpolation::Linear);
    CHECK(sizeBack.sizeY.Evaluate(0.5f) == doctest::Approx(2.5f));
    particles::ColorOverLifetimeBehavior colorBack;
    REQUIRE(ReadXml(reinterpret_cast<const char8_t*>(colorText.Data()), [&](ISerializer& ar) { colorBack.Serialize(ar); }));
    CHECK(colorBack.curve.interpolation == GradientInterpolation::OkLab);
    CHECK(colorBack.curve.alphaCount == 3);
    CHECK(colorBack.curve.colors[0].color.b == doctest::Approx(0.6f));
}
