// AnimationGraphEditorPage tests (headless): the "New Animation Graph" seed is a free, pure
// function, and the ASSET (source + editor-only canvas layout) round-trips through the binary
// serializer - the same blob path the page's undo snapshots ride. The canvas/inspector wiring
// needs a live host and is exercised in the editor app.

#include <doctest/doctest.h>

#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.editor.scene;

using namespace draconic::foundation;
namespace anim = draconic::animation;

TEST_CASE("animation graph page: default seed = one layer, Idle default state, Speed param")
{
    anim::AnimationGraphAsset asset;
    draconic::editor::SeedDefaultAnimationGraph(asset);

    REQUIRE(asset.source.paramNames.Size() == 1u);
    CHECK(asset.source.paramNames[0].AsView() == u8"Speed");
    CHECK(asset.source.paramTypes[0] == 0); // Float

    REQUIRE(asset.source.layers.Size() == 1u);
    const anim::GraphLayerData& layer = asset.source.layers[0];
    CHECK(layer.name.AsView() == u8"Base");
    REQUIRE(layer.states.Size() == 1u);
    CHECK(layer.states[0].name.AsView() == u8"Idle");
    CHECK(layer.states[0].node.kind == 0); // clip state
    CHECK(layer.defaultState == 0);
    CHECK(layer.transitions.IsEmpty());

    // The editor-only canvas layout is parallel to the source.
    REQUIRE(asset.layerStatePositions.Size() == 1u);
    REQUIRE(asset.layerStatePositions[0].Size() == 1u);
    REQUIRE(asset.layerAnyStatePositions.Size() == 1u);
}

TEST_CASE("animation graph page: asset round-trips source + canvas layout (undo blob path)")
{
    anim::AnimationGraphAsset a;
    draconic::editor::SeedDefaultAnimationGraph(a);
    // Author a bit beyond the seed: a second state, a transition with a condition, a moved node.
    anim::GraphStateData run;
    run.name = String(u8"Run");
    run.node.kind = 1; // blend 1d
    run.node.paramIndex = 0;
    run.node.entryClips.PushBack(Guid{0xAB, 0x12});
    run.node.entryThresholds.PushBack(0.5f);
    a.source.layers[0].states.PushBack(Move(run));
    anim::GraphTransitionData t;
    t.src = 0;
    t.dst = 1;
    t.duration = 0.15f;
    anim::GraphConditionData c;
    c.paramIndex = 0;
    c.op = 2; // Greater
    c.threshold = 0.1f;
    t.conditions.PushBack(c);
    a.source.layers[0].transitions.PushBack(Move(t));
    a.layerStatePositions[0].PushBack(Float2{420.0f, 200.0f});
    a.layerAnyStatePositions[0] = Float2{25.0f, 75.0f};

    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        a.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    anim::AnimationGraphAsset b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        b.Serialize(ar);
        REQUIRE(ar.IsOk());
    }

    REQUIRE(b.source.layers.Size() == 1u);
    const anim::GraphLayerData& layer = b.source.layers[0];
    REQUIRE(layer.states.Size() == 2u);
    CHECK(layer.states[1].name.AsView() == u8"Run");
    CHECK(layer.states[1].node.kind == 1);
    CHECK(layer.states[1].node.paramIndex == 0);
    REQUIRE(layer.states[1].node.entryClips.Size() == 1u);
    CHECK(layer.states[1].node.entryClips[0] == Guid{0xAB, 0x12});
    CHECK(layer.states[1].node.entryThresholds[0] == doctest::Approx(0.5f));
    REQUIRE(layer.transitions.Size() == 1u);
    CHECK(layer.transitions[0].src == 0);
    CHECK(layer.transitions[0].dst == 1);
    CHECK(layer.transitions[0].duration == doctest::Approx(0.15f));
    REQUIRE(layer.transitions[0].conditions.Size() == 1u);
    CHECK(layer.transitions[0].conditions[0].op == 2);
    CHECK(layer.transitions[0].conditions[0].threshold == doctest::Approx(0.1f));

    // The canvas layout came through - editor-only, but part of the ASSET stream.
    REQUIRE(b.layerStatePositions.Size() == 1u);
    REQUIRE(b.layerStatePositions[0].Size() == 2u);
    CHECK(b.layerStatePositions[0][1].x == doctest::Approx(420.0f));
    CHECK(b.layerAnyStatePositions[0].y == doctest::Approx(75.0f));
}

TEST_CASE("animation clip page: asset round-trips loop flag + events (undo blob path)")
{
    anim::AnimationClipAsset a;
    a.source.name = String(u8"Walk");
    a.source.duration = 1.25f;
    a.source.isLooping = true;
    a.source.eventTimes.PushBack(0.4f);
    a.source.eventNames.PushBack(String(u8"footstep"));
    a.source.eventTimes.PushBack(0.9f);
    a.source.eventNames.PushBack(String(u8"footstep"));

    MemoryStream stream;
    {
        BinarySerializer ar(stream, SerializeMode::Write);
        a.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    (void)stream.Seek(0, SeekOrigin::Begin);
    anim::AnimationClipAsset b;
    {
        BinarySerializer ar(stream, SerializeMode::Read);
        b.Serialize(ar);
        REQUIRE(ar.IsOk());
    }
    CHECK(b.source.name.AsView() == u8"Walk");
    CHECK(b.source.duration == doctest::Approx(1.25f));
    CHECK(b.source.isLooping);
    REQUIRE(b.source.eventTimes.Size() == 2u);
    CHECK(b.source.eventTimes[1] == doctest::Approx(0.9f));
    CHECK(b.source.eventNames[1].AsView() == u8"footstep");
}

TEST_CASE("skeleton page: SkeletonStatLines reports bones/roots/depth")
{
    // root -> spine -> head; root -> leg
    anim::Skeleton skeleton{4};
    const StringView names[4] = {u8"root", u8"spine", u8"head", u8"leg"};
    const i32 parents[4] = {-1, 0, 1, 0};
    for (i32 i = 0; i < 4; ++i)
    {
        skeleton.Bones()[static_cast<usize>(i)].index = i;
        skeleton.Bones()[static_cast<usize>(i)].name = String(names[i]);
        skeleton.Bones()[static_cast<usize>(i)].parentIndex = parents[i];
    }
    skeleton.FindRootBones();
    skeleton.BuildChildIndices();

    const Array<String> lines = draconic::editor::SkeletonStatLines(skeleton);
    bool sawBones = false, sawRoots = false, sawDepth = false;
    for (const String& line : lines)
    {
        if (line.AsView() == u8"4 bones")
        {
            sawBones = true;
        }
        if (line.AsView() == u8"1 roots")
        {
            sawRoots = true;
        }
        if (line.AsView() == u8"depth 2")
        {
            sawDepth = true;
        }
    }
    CHECK(sawBones);
    CHECK(sawRoots);
    CHECK(sawDepth);
}
