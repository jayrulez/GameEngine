// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// An animation graph as a graph source (Editor.Animation's AnimationGraphStates), no widgets: a
// layer's states and Any State its nodes, its transitions node links; a transition into Any State
// or into its own state refused; each change one edit; a state deleted taking its transitions and
// moving the indices after it down while every other id stays; a layer keeping one state; a move
// one step, a cancelled one none; and the graph edits the page's lists share (a parameter removed,
// a layer added and removed).

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.animation;

using namespace foundation::core;
using namespace editor;
namespace gui = foundation::gui;
namespace animation = foundation::animation;

namespace
{
    /// Locomotion (the default), Attack and Die; Locomotion to Attack on the Attack trigger,
    /// Attack back at its end, Any State to Die while Dead; Speed drives nothing yet.
    struct Bed
    {
        pipeline::AnimationGraphAsset asset;
        int edits = 0;
        UniquePtr<AnimationGraphStates> states;

        Bed()
        {
            auto& source = asset.source;
            (void)animgraph::AddParameter(asset, u8"Speed", animation::AnimationParameterType::Float);
            (void)animgraph::AddParameter(asset, u8"Attack", animation::AnimationParameterType::Trigger);
            (void)animgraph::AddParameter(asset, u8"Dead", animation::AnimationParameterType::Bool);
            animation::GraphLayerData layer;
            layer.name = String(u8"Base");
            source.layers.PushBack(Move(layer));
            (void)animgraph::AddState(asset, 0, u8"Locomotion", animgraph::StateKind::Blend1D, Float2{300.0f, 200.0f});
            (void)animgraph::AddState(asset, 0, u8"Attack", animgraph::StateKind::Clip, Float2{600.0f, 80.0f});
            (void)animgraph::AddState(asset, 0, u8"Die", animgraph::StateKind::Clip, Float2{60.0f, 320.0f});
            source.layers[0].states[0].node.paramIndex = 0;
            Transition(0, 1, 1);
            Transition(1, 0, -1);
            Transition(-1, 2, 2);
            states = MakeUnique<AnimationGraphStates>(DefaultAllocator(), [this]() { return &asset; },
                                                      [this](StringView, const Function<void(pipeline::AnimationGraphAsset&)>& mutate)
                                                      {
                                                          ++edits;
                                                          mutate(asset);
                                                      });
        }
        void Transition(i32 src, i32 dst, i32 parameter)
        {
            animation::GraphTransitionData transition;
            transition.src = src;
            transition.dst = dst;
            if (parameter >= 0)
                transition.conditions.PushBack(animation::GraphConditionData{parameter, 0, 1.0f});
            asset.source.layers[0].transitions.PushBack(Move(transition));
        }
        animation::GraphLayerData& Layer() { return asset.source.layers[0]; }
        gui::GraphLinkInfo NodeLink(gui::GraphId from, gui::GraphId to)
        {
            gui::GraphLinkInfo link;
            link.Kind = gui::GraphLinkKind::Nodes;
            link.FromNode = from;
            link.ToNode = to;
            return link;
        }
    };
}

TEST_CASE("graph states: the states and Any State are the nodes, the transitions node links")
{
    Bed bed;
    Array<gui::GraphId> nodes;
    bed.states->GetNodes(nodes);
    REQUIRE(nodes.Size() == 4u);
    CHECK(nodes[0] == AnimationGraphStates::kAnyState);
    const gui::GraphNodeInfo* any = bed.states->GetNode(AnimationGraphStates::kAnyState);
    REQUIRE(any != nullptr);
    CHECK(any->Title == String(u8"Any State"));
    CHECK_FALSE(any->Deletable);
    const gui::GraphNodeInfo* locomotion = bed.states->GetNode(bed.states->StateId(0));
    REQUIRE(locomotion != nullptr);
    CHECK(locomotion->Title == String(u8"Locomotion"));
    CHECK(locomotion->Subtitle == String(u8"Starts here · Blends 0 by Speed"));
    CHECK(locomotion->Position.x == doctest::Approx(300.0f));
    CHECK(bed.states->GetNode(bed.states->StateId(1))->Subtitle == String(u8"No clip"));

    Array<gui::GraphLinkInfo> links;
    bed.states->GetLinks(links);
    REQUIRE(links.Size() == 3u);
    CHECK(links[0].Kind == gui::GraphLinkKind::Nodes);
    CHECK(links[0].FromNode == bed.states->StateId(0));
    CHECK(links[0].ToNode == bed.states->StateId(1));
    CHECK(links[2].FromNode == AnimationGraphStates::kAnyState);
    CHECK(links[2].ToNode == bed.states->StateId(2));
}

TEST_CASE("graph states: a transition into Any State or into its own state is refused; one made is one edit")
{
    Bed bed;
    const gui::GraphId die = bed.states->StateId(2);
    const gui::GraphId attack = bed.states->StateId(1);
    CHECK(bed.states->CheckLink(bed.NodeLink(die, 0)).Allowed); // one may begin at a state
    CHECK(bed.states->CheckLink(bed.NodeLink(AnimationGraphStates::kAnyState, 0)).Allowed);
    CHECK_FALSE(bed.states->CheckLink(bed.NodeLink(die, AnimationGraphStates::kAnyState)).Allowed);
    CHECK_FALSE(bed.states->CheckLink(bed.NodeLink(die, die)).Allowed);
    CHECK(bed.states->Connect(bed.NodeLink(die, die)) == 0);
    CHECK(bed.edits == 0);

    const gui::GraphId made = bed.states->Connect(bed.NodeLink(die, bed.states->StateId(0)));
    CHECK(made != 0);
    CHECK(bed.edits == 1);
    REQUIRE(bed.Layer().transitions.Size() == 4u);
    CHECK(bed.Layer().transitions[3].src == 2);
    CHECK(bed.Layer().transitions[3].dst == 0);
    CHECK(bed.states->TransitionOf(made) == 3);

    // Relinked: its ends move, its id stays.
    CHECK(bed.states->Relink(made, bed.NodeLink(attack, die)));
    CHECK(bed.Layer().transitions[3].src == 1);
    CHECK(bed.Layer().transitions[3].dst == 2);
    CHECK(bed.states->TransitionOf(made) == 3);

    // Taken away: the others keep their ids.
    const gui::GraphId first = bed.states->TransitionId(0);
    const gui::GraphId gone = bed.states->TransitionId(1);
    CHECK(bed.states->Disconnect(Span<const gui::GraphId>(&gone, 1u)));
    CHECK(bed.Layer().transitions.Size() == 3u);
    CHECK(bed.states->TransitionOf(first) == 0);
    CHECK(bed.states->TransitionOf(made) == 2);
    CHECK(bed.edits == 3);
}

TEST_CASE("graph states: a state deleted takes its transitions and moves those after it down; a layer keeps one")
{
    Bed bed;
    const gui::GraphId locomotion = bed.states->StateId(0);
    const gui::GraphId attack = bed.states->StateId(1);
    const gui::GraphId die = bed.states->StateId(2);
    const gui::GraphId toDie = bed.states->TransitionId(2);
    CHECK(bed.states->DeleteNodes(Span<const gui::GraphId>(&attack, 1u)));
    CHECK(bed.edits == 1);
    REQUIRE(bed.Layer().states.Size() == 2u);
    CHECK(bed.Layer().states[1].name == String(u8"Die"));
    REQUIRE(bed.Layer().transitions.Size() == 1u); // both of Attack's went
    CHECK(bed.Layer().transitions[0].src == -1);
    CHECK(bed.Layer().transitions[0].dst == 1); // Die moved down
    CHECK(bed.states->StateOf(die) == 1);       // its id stayed with it
    CHECK(bed.states->StateOf(locomotion) == 0);
    CHECK(bed.states->TransitionOf(toDie) == 0);
    CHECK(bed.asset.layerStatePositions[0].Size() == 2u);
    CHECK(bed.asset.layerStatePositions[0][1].x == doctest::Approx(60.0f));

    // Any State is never deleted; nor is a layer's last state.
    const gui::GraphId everything[] = {AnimationGraphStates::kAnyState, locomotion, die};
    CHECK(bed.states->DeleteNodes(Span<const gui::GraphId>(everything, 3u)));
    REQUIRE(bed.Layer().states.Size() == 1u);
    CHECK(bed.Layer().states[0].name == String(u8"Locomotion"));
    CHECK(bed.Layer().defaultState == 0);
    CHECK(bed.Layer().transitions.IsEmpty());
    CHECK_FALSE(bed.states->DeleteNodes(Span<const gui::GraphId>(&locomotion, 1u)));
}

TEST_CASE("graph states: a move is one step, a cancelled one none; a new state named apart")
{
    Bed bed;
    const gui::GraphId attack = bed.states->StateId(1);
    const gui::GraphId moving[] = {attack, AnimationGraphStates::kAnyState};
    bed.states->BeginMove();
    bed.states->PreviewMove(Span<const gui::GraphId>(moving, 2u), Span<const gui::GraphId>{}, Float2{10.0f, 5.0f});
    bed.states->PreviewMove(Span<const gui::GraphId>(moving, 2u), Span<const gui::GraphId>{}, Float2{20.0f, 15.0f});
    CHECK(bed.states->GetNode(attack)->Position.x == doctest::Approx(620.0f)); // live
    CHECK(bed.edits == 0);
    bed.states->CommitMove();
    CHECK(bed.edits == 1);
    CHECK(bed.asset.layerStatePositions[0][1].x == doctest::Approx(620.0f));
    CHECK(bed.asset.layerStatePositions[0][1].y == doctest::Approx(95.0f));
    CHECK(bed.asset.layerAnyStatePositions[0].x == doctest::Approx(60.0f));

    bed.states->BeginMove();
    bed.states->PreviewMove(Span<const gui::GraphId>(&attack, 1u), Span<const gui::GraphId>{}, Float2{100.0f, 0.0f});
    bed.states->CancelMove();
    CHECK(bed.asset.layerStatePositions[0][1].x == doctest::Approx(620.0f));
    CHECK(bed.edits == 1);

    const gui::GraphId made = bed.states->CreateNode(u8"clip", Float2{50.0f, 60.0f}, 0);
    CHECK(made != 0);
    CHECK(bed.states->CreateNode(u8"clip", Float2{90.0f, 60.0f}, 0) != 0);
    CHECK(bed.states->CreateNode(u8"blend1d", Float2{90.0f, 60.0f}, 0) != 0);
    CHECK(bed.states->CreateNode(u8"something else", Float2{}, 0) == 0);
    REQUIRE(bed.Layer().states.Size() == 6u);
    CHECK(bed.Layer().states[3].name == String(u8"State"));
    CHECK(bed.Layer().states[4].name == String(u8"State 2"));
    CHECK(bed.Layer().states[5].name == String(u8"Blend"));
    CHECK(bed.Layer().states[5].node.kind == static_cast<u8>(animgraph::StateKind::Blend1D));
    CHECK(bed.states->StateOf(made) == 3);
    CHECK(bed.asset.layerStatePositions[0][3].x == doctest::Approx(50.0f));
}

TEST_CASE("graph edits: a parameter removed takes its conditions and drivers; layers added and removed")
{
    Bed bed;
    REQUIRE(animgraph::RemoveParameter(bed.asset, 0)); // Speed: the blend's driver
    CHECK(bed.asset.source.paramNames.Size() == 2u);
    CHECK(bed.asset.source.paramTypes.Size() == 2u);
    CHECK(bed.Layer().states[0].node.paramIndex == -1);
    CHECK(bed.Layer().transitions[0].conditions[0].paramIndex == 0); // Attack moved down
    CHECK(bed.Layer().transitions[2].conditions[0].paramIndex == 1); // Dead moved down
    REQUIRE(animgraph::RemoveParameter(bed.asset, 0)); // Attack: its condition goes
    CHECK(bed.Layer().transitions[0].conditions.IsEmpty());
    CHECK(bed.Layer().transitions[2].conditions[0].paramIndex == 0);
    CHECK(animgraph::AddParameter(bed.asset, u8"Dead", animation::AnimationParameterType::Bool) == 1);
    CHECK(bed.asset.source.paramNames[1] == String(u8"Dead 2"));

    CHECK(animgraph::AddLayer(bed.asset, u8"Base") == 1);
    CHECK(bed.asset.source.layers[1].name == String(u8"Base 2"));
    CHECK(bed.asset.source.layers[1].states.Size() == 1u);
    CHECK(bed.asset.layerStatePositions.Size() == 2u);
    CHECK(bed.asset.layerAnyStatePositions.Size() == 2u);
    CHECK(animgraph::RemoveLayer(bed.asset, 0));
    CHECK(bed.asset.source.layers[0].name == String(u8"Base 2"));
    CHECK_FALSE(animgraph::RemoveLayer(bed.asset, 0)); // the last stays
}
