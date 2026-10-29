// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Effects (Specs/wind-for-every-shader.md): which materials select an effect's variant (an
// effect group whose gate is above zero turns its flag on), the flag's name in the variant table,
// and PBR's buffer, which no longer carries the wind (the block stays 64 bytes, its former wind
// lanes padding).
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.shaders;
import foundation.materials;
import foundation.render;

using namespace foundation::core;
using namespace foundation::render;
namespace materials = foundation::materials;
namespace shaders = foundation::shaders;

namespace
{
    // An effect's table as the shader system serves it: parsed from an effect block (the
    // engine's own is wind.hlsli's; this test is of the rule, not of wind's parameters).
    shaders::ShaderEffect WindTable()
    {
        const shaders::ShaderEffectBlock block = shaders::ParseShaderEffect(
            u8"effect Wind : WIND\n"
            u8"{\n"
            u8"    float Strength = 0 [0, 2] (gate);\n"
            u8"    float Speed = 1 [0, 20];\n"
            u8"    float Height = 1 [0.01, 100];\n"
            u8"}\n");
        REQUIRE(block.IsOk());
        return block.effect;
    }
}

TEST_CASE("effects: a material turns an effect's variant on through its group's gate above zero")
{
    CHECK(MeshRenderer::MaterialEffectFlags(nullptr) == shaders::ShaderFlags::None);

    const shaders::ShaderEffect wind = WindTable();
    RefPtr<materials::Material> plain = materials::CreatePBR(u8"plain");
    plain->AddEffectGroup(wind);
    CHECK(MeshRenderer::MaterialEffectFlags(plain.Get()) == shaders::ShaderFlags::None); // gate 0: off

    RefPtr<materials::Material> card = materials::CreatePBR(u8"card");
    card->AddEffectGroup(wind);
    card->SetDefaultFloat(u8"Wind.Strength", 0.2f);
    CHECK(MeshRenderer::MaterialEffectFlags(card.Get()) == shaders::ShaderFlags::Wind);
    card->SetDefaultFloat(u8"Wind.Speed", 5.0f); // not the gate: the variant is the gate's alone
    card->SetDefaultFloat(u8"Wind.Strength", 0.0f);
    CHECK(MeshRenderer::MaterialEffectFlags(card.Get()) == shaders::ShaderFlags::None);

    // A material without the group (built without the table, or on a shader that cannot sway)
    // never sways, and the old property name reaches nothing.
    RefPtr<materials::Material> unlit = materials::CreateUnlit(u8"unlit");
    unlit->SetDefaultFloat(u8"WindStrength", 1.0f);
    unlit->SetDefaultFloat(u8"Wind.Strength", 1.0f);
    CHECK(MeshRenderer::MaterialEffectFlags(unlit.Get()) == shaders::ShaderFlags::None);

    // The rule is the effect's flag, whatever the effect: a group whose table names another flag
    // turns that one on.
    shaders::ShaderEffect fade = wind;
    fade.name = String(u8"Fade");
    fade.flag = shaders::ShaderFlags::Dither;
    RefPtr<materials::Material> both = materials::CreatePBR(u8"both");
    both->AddEffectGroup(wind);
    both->AddEffectGroup(fade);
    both->SetDefaultFloat(u8"Fade.Strength", 1.0f);
    CHECK(MeshRenderer::MaterialEffectFlags(both.Get()) == shaders::ShaderFlags::Dither);
}

TEST_CASE("effects: PBR's buffer has no wind lanes; the block stays 64 bytes, its other offsets unmoved")
{
    RefPtr<materials::Material> pbr = materials::CreatePBR(u8"pbr");
    CHECK(pbr->FindProperty(u8"WindStrength") == nullptr);
    CHECK(pbr->FindProperty(u8"WindSpeed") == nullptr);
    CHECK(pbr->FindProperty(u8"WindHeight") == nullptr);
    CHECK(pbr->FindProperty(u8"Roughness")->offset == 20u);
    CHECK(pbr->FindProperty(u8"EmissiveColor")->offset == 32u); // 24 and 28 are padding
    CHECK(pbr->FindProperty(u8"AlphaCutoff")->offset == 56u);   // 60 is padding
    CHECK(pbr->UniformDataSize() == 64u);
    CHECK(pbr->EffectGroupCount() == 0u); // built without the effect's table: no group

    // The group's offsets are the effect's, in its own buffer.
    pbr->AddEffectGroup(WindTable());
    REQUIRE(pbr->EffectGroupCount() == 1u);
    const materials::MaterialEffectGroup& group = pbr->GetEffectGroup(0);
    CHECK(group.name == u8"Wind");
    CHECK(group.flag == shaders::ShaderFlags::Wind);
    CHECK(group.uniformSize == 16u);
    REQUIRE(group.Gate() != nullptr);
    CHECK(group.Gate()->name == u8"Strength");
    CHECK(group.FindProperty(u8"Speed")->offset == 4u);
    CHECK(group.FindProperty(u8"Height")->offset == 8u);
    CHECK(pbr->UniformDataSize() == 64u); // the shader's buffer untouched

    // The flag names its #define, so a stage's `variants:` line can declare it.
    CHECK(shaders::FlagFromName(u8"WIND") == shaders::ShaderFlags::Wind);
}

TEST_CASE("wind: the snapshot carries its scene's clock; a reset clears it (the frame clock stands in)")
{
    ExtractedScene scene{DefaultAllocator()};
    CHECK(!scene.HasTime());
    scene.SetTime(12.5f, 12.4f);
    CHECK(scene.HasTime());
    CHECK(scene.TimeSeconds() == 12.5f);
    CHECK(scene.PrevTimeSeconds() == 12.4f);
    scene.Reset();
    CHECK(!scene.HasTime());
    CHECK(scene.TimeSeconds() == 0.0f);
}

// The depth-like passes (the depth prepass, the shadow maps, the pick): the built-in programs, or
// a material's own when its shader has them, which then bind the material's set 2 they were
// generated for.
TEST_CASE("depth passes: a material's own depth and pick programs replace the built-ins")
{
    RenderRecordContext ctx{};
    RefPtr<materials::Material> plain = materials::CreatePBR(u8"plain");
    const MeshRenderer::DepthLikeChoice builtIn =
        MeshRenderer::DepthLikeFor(ctx, plain.Get(), /*instanced*/ false, /*pick*/ false);
    CHECK(builtIn.config.shaderName == u8"shadow_depth");
    CHECK_FALSE(builtIn.materialSet); // opaque, still: no set 2
    CHECK(builtIn.config.depthOnly);

    RefPtr<materials::Material> waves = materials::CreatePBR(u8"waves");
    waves->depthShaderName = String(u8"waves.depth");
    waves->pickShaderName = String(u8"waves.pick");
    waves->pipeline.cullMode = materials::CullModeConfig::None;
    const MeshRenderer::DepthLikeChoice depth =
        MeshRenderer::DepthLikeFor(ctx, waves.Get(), /*instanced*/ true, /*pick*/ false);
    CHECK(depth.config.shaderName == u8"waves.depth");
    CHECK(depth.materialSet);
    CHECK(shaders::HasFlag(depth.config.shaderFlags, shaders::ShaderFlags::Instanced));
    CHECK(depth.config.depthOnly); // opaque: its vertex stage alone
    const MeshRenderer::DepthLikeChoice pick =
        MeshRenderer::DepthLikeFor(ctx, waves.Get(), /*instanced*/ false, /*pick*/ true);
    CHECK(pick.config.shaderName == u8"waves.pick");
    CHECK(pick.config.cullMode == materials::CullModeConfig::None); // as the material draws

    // Masked: the cut-out's fragment runs, over the material's set 2.
    waves->pipeline.blendMode = materials::BlendMode::Masked;
    const MeshRenderer::DepthLikeChoice masked =
        MeshRenderer::DepthLikeFor(ctx, waves.Get(), false, false);
    CHECK_FALSE(masked.config.depthOnly);
    CHECK(shaders::HasFlag(masked.config.shaderFlags, shaders::ShaderFlags::AlphaTest));

    // No material: the built-in.
    CHECK(MeshRenderer::DepthLikeFor(ctx, nullptr, false, true).config.shaderName == u8"pick_ids");
}

TEST_CASE("depth passes: an active effect selects its variant and binds the material's set")
{
    // The effect's block is in the material's set (b1), so the shadow and the pick of a swaying
    // draw bind it, with the built-in programs, which read the same block the forward stage does.
    RenderRecordContext ctx{};
    RefPtr<materials::Material> card = materials::CreatePBR(u8"card");
    card->AddEffectGroup(WindTable());
    const MeshRenderer::DepthLikeChoice still = MeshRenderer::DepthLikeFor(ctx, card.Get(), false, false);
    CHECK_FALSE(still.materialSet);
    CHECK_FALSE(shaders::HasFlag(still.config.shaderFlags, shaders::ShaderFlags::Wind));

    card->SetDefaultFloat(u8"Wind.Strength", 0.3f);
    for (int pass = 0; pass < 2; ++pass)
    {
        const bool pick = pass == 1;
        const MeshRenderer::DepthLikeChoice sway = MeshRenderer::DepthLikeFor(ctx, card.Get(), true, pick);
        CHECK(sway.materialSet);
        CHECK(shaders::HasFlag(sway.config.shaderFlags, shaders::ShaderFlags::Wind));
        CHECK(sway.config.shaderName == (pick ? StringView(u8"pick_ids") : StringView(u8"shadow_depth")));
    }
}
