// Animation graph stack: parameters, conditions, transitions, states, layers, blend trees, graph,
// and a graph-player smoke test. Ports Sedulous.Animation.Tests (AnimationGraph*, BlendTree*,
// BoneMask, AnimationLayer) + adds a state-machine integration check.
#include <doctest/doctest.h>
#include "Draconic.Foundation/Prelude.h"

import draconic.foundation;
import draconic.animation;

using namespace draconic::foundation;
using namespace draconic::animation;

static IAnimationStateNode* kNullNode = static_cast<IAnimationStateNode*>(nullptr);

// ---- BoneMask ----
TEST_CASE("graph: BoneMask weights, clamping, out-of-range")
{
    BoneMask mask{4, 1.0f};
    CHECK(mask.BoneCount() == 4);
    CHECK(mask.GetWeight(0) == 1.0f);
    mask.SetWeight(2, 0.75f);
    CHECK(mask.GetWeight(2) == 0.75f);
    mask.SetWeight(0, 2.0f);
    CHECK(mask.GetWeight(0) == 1.0f); // clamp high
    mask.SetWeight(1, -1.0f);
    CHECK(mask.GetWeight(1) == 0.0f);  // clamp low
    CHECK(mask.GetWeight(-1) == 0.0f); // out of range
    CHECK(mask.GetWeight(99) == 0.0f);
    BoneMask m2{2, 0.0f};
    m2.SetAll(5.0f);
    CHECK(m2.GetWeight(0) == 1.0f); // SetAll clamps
}

TEST_CASE("graph: BoneMask SetBoneChainWeight follows hierarchy")
{
    Skeleton s{3};
    s.Bones()[0].index = 0;
    s.Bones()[0].parentIndex = -1;
    s.Bones()[1].index = 1;
    s.Bones()[1].parentIndex = 0;
    s.Bones()[2].index = 2;
    s.Bones()[2].parentIndex = 1;
    s.FindRootBones();
    s.BuildChildIndices();
    BoneMask mask{3, 0.0f};
    mask.SetBoneChainWeight(s, 0, 1.0f); // root + all descendants
    CHECK(mask.GetWeight(0) == 1.0f);
    CHECK(mask.GetWeight(1) == 1.0f);
    CHECK(mask.GetWeight(2) == 1.0f);
}

// ---- AnimationGraphParameter ----
TEST_CASE("graph: parameter set/get + trigger consume")
{
    AnimationGraphParameter sp{u8"Speed", AnimationParameterType::Float};
    CHECK(sp.floatValue == 0.0f);
    CHECK(sp.type == AnimationParameterType::Float);
    CHECK(sp.Name() == StringView{u8"Speed"});
    sp.floatValue = 1.5f;
    CHECK(sp.floatValue == 1.5f);

    AnimationGraphParameter trig{u8"Fire", AnimationParameterType::Trigger};
    trig.boolValue = true;
    trig.ConsumeTrigger();
    CHECK(trig.boolValue == false); // trigger resets

    AnimationGraphParameter b{u8"Flag", AnimationParameterType::Bool};
    b.boolValue = true;
    b.ConsumeTrigger();
    CHECK(b.boolValue == true); // bool not affected
}

// ---- AnimationGraphCondition ----
TEST_CASE("graph: condition float/int/bool comparisons")
{
    AnimationGraphParameter sp{u8"Speed", AnimationParameterType::Float};
    sp.floatValue = 0.5f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 0.1f}.Evaluate(&sp) == true);
    sp.floatValue = 0.05f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 0.1f}.Evaluate(&sp) == false);
    sp.floatValue = 0.1f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::LessEqual, 0.1f}.Evaluate(&sp) == true);
    sp.floatValue = 1.0f;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Equal, 1.0f}.Evaluate(&sp) == true);

    AnimationGraphParameter lvl{u8"Level", AnimationParameterType::Int};
    lvl.intValue = 5;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 3.0f}.Evaluate(&lvl) == true);

    AnimationGraphParameter g{u8"Grounded", AnimationParameterType::Bool};
    g.boolValue = true;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Equal, 1.0f}.Evaluate(&g) == true); // "is true"
    g.boolValue = false;
    CHECK(AnimationGraphCondition{0, ComparisonOp::Equal, 0.0f}.Evaluate(&g) == true); // "is false"

    CHECK(AnimationGraphCondition{0, ComparisonOp::Greater, 0.0f}.Evaluate(nullptr) == false);
}

// ---- AnimationGraphTransition ----
TEST_CASE("graph: transition condition evaluation")
{
    Array<AnimationGraphParameter> params;
    params.PushBack(AnimationGraphParameter{u8"Speed", AnimationParameterType::Float});
    params.PushBack(AnimationGraphParameter{u8"Grounded", AnimationParameterType::Bool});
    params[0].floatValue = 1.0f;
    params[1].boolValue = true;
    const Span<const AnimationGraphParameter> pv{params.Data(), params.Size()};

    AnimationGraphTransition t;
    CHECK(t.sourceStateIndex == -1); // defaults
    CHECK(t.duration == 0.25f);
    CHECK(t.EvaluateConditions(pv) == true); // no conditions -> unconditional

    t.AddFloatCondition(0, ComparisonOp::Greater, 0.1f);
    t.AddBoolCondition(1, true);
    CHECK(t.EvaluateConditions(pv) == true); // both met
    params[1].boolValue = false;
    CHECK(t.EvaluateConditions(pv) == false); // one fails

    AnimationGraphTransition bad;
    bad.AddFloatCondition(5, ComparisonOp::Greater, 0.0f); // index out of range
    CHECK(bad.EvaluateConditions(pv) == false);
}

// ---- AnimationGraphState ----
TEST_CASE("graph: state defaults + node ownership")
{
    AnimationGraphState idle{u8"Idle", kNullNode};
    CHECK(idle.Name() == StringView{u8"Idle"});
    CHECK(idle.Node() == nullptr);
    CHECK(idle.speed == 1.0f);
    CHECK(idle.loop == true);
    CHECK(idle.OwnsNode() == false);
    CHECK(idle.Duration() == 0.0f);

    // Owned node freed on destruction (UniquePtr) - no leak/crash.
    {
        AnimationGraphState owned{
            u8"Owned",
            MakeUnique<ClipStateNode>(DefaultAllocator(), static_cast<AnimationClip*>(nullptr))};
        CHECK(owned.OwnsNode() == true);
    }
    // Borrowed node survives the state.
    ClipStateNode node{nullptr};
    {
        AnimationGraphState borrow{u8"Borrow", &node};
        CHECK(borrow.OwnsNode() == false);
    }
    CHECK(node.Duration() == 0.0f);
}

// ---- AnimationLayer ----
TEST_CASE("graph: layer states/transitions/mask")
{
    AnimationLayer layer{u8"Base"};
    CHECK(layer.Name() == StringView{u8"Base"});
    CHECK(layer.defaultStateIndex == 0);
    CHECK(layer.blendMode == LayerBlendMode::Override);
    CHECK(layer.weight == 1.0f);
    CHECK(layer.Mask() == nullptr);

    CHECK(layer.AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Idle"},
                                                         kNullNode)) == 0);
    CHECK(layer.AddState(MakeUnique<AnimationGraphState>(DefaultAllocator(), StringView{u8"Walk"},
                                                         kNullNode)) == 1);
    CHECK(layer.States().Size() == 2);
    CHECK(layer.GetState(1)->Name() == StringView{u8"Walk"});
    CHECK(layer.GetState(-1) == nullptr);
    CHECK(layer.GetState(99) == nullptr);
    CHECK(layer.FindStateIndex(u8"Walk") == 1);

    layer.AddTransition(MakeUnique<AnimationGraphTransition>(DefaultAllocator()));
    CHECK(layer.Transitions().Size() == 1);

    layer.SetMask(MakeUnique<BoneMask>(DefaultAllocator(), 4, 0.0f));
    layer.Mask()->SetWeight(0, 1.0f);
    CHECK(layer.Mask()->GetWeight(0) == 1.0f);
    CHECK(layer.Mask()->GetWeight(1) == 0.0f);
}

// ---- BlendTree1D / 2D ----
TEST_CASE("graph: BlendTree1D sorts by threshold, duration, empty-safe")
{
    BlendTree1D tree;
    tree.AddEntry(1.0f, nullptr);
    tree.AddEntry(0.0f, nullptr);
    tree.AddEntry(0.5f, nullptr);
    CHECK(tree.Entries().Size() == 3);
    CHECK(tree.Entries()[0].threshold == 0.0f);
    CHECK(tree.Entries()[1].threshold == 0.5f);
    CHECK(tree.Entries()[2].threshold == 1.0f);
    CHECK(tree.parameter == 0.0f);
    CHECK(tree.Duration() == 0.0f); // null clips
    tree.AddEntry(0.5f, nullptr);
    CHECK(tree.Entries().Size() == 4); // duplicates allowed

    Skeleton empty{0};
    BoneTransform poses[4] = {};
    BlendTree1D blank;
    blank.Evaluate(empty, 0.0f, Span<BoneTransform>{poses, 4}); // empty -> no crash
}

TEST_CASE("graph: BlendTree2D entries + positions + duration")
{
    BlendTree2D tree;
    tree.AddEntry(0.0f, 0.0f, nullptr);
    tree.AddEntry(Float2{1, 0}, nullptr);
    tree.AddEntry(-1.0f, 2.5f, nullptr);
    CHECK(tree.Entries().Size() == 3);
    CHECK(tree.Entries()[1].position.x == 1.0f);
    CHECK(tree.Entries()[2].position.x == -1.0f);
    CHECK(tree.Entries()[2].position.y == 2.5f);
    CHECK(tree.parameterX == 0.0f);
    CHECK(tree.Duration() == 0.0f);
}

// ---- AnimationGraph ----
TEST_CASE("graph: parameters + layers + find (case sensitive)")
{
    AnimationGraph graph;
    CHECK(graph.AddParameter(u8"Speed", AnimationParameterType::Float) == 0);
    CHECK(graph.AddParameter(u8"Grounded", AnimationParameterType::Bool) == 1);
    CHECK(graph.Parameters().Size() == 2);
    CHECK(graph.FindParameter(u8"Speed") == 0);
    CHECK(graph.FindParameter(u8"Missing") == -1);
    CHECK(graph.FindParameter(u8"speed") == -1); // case sensitive
    CHECK(graph.GetParameter(0)->type == AnimationParameterType::Float);
    CHECK(graph.GetParameter(-1) == nullptr);

    CHECK(graph.AddLayer(MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"})) ==
          0);
    CHECK(graph.AddLayer(MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Upper"})) ==
          1);
    CHECK(graph.Layers().Size() == 2);
}

// ---- AnimationGraphPlayer (state-machine integration) ----
TEST_CASE("graph: player transitions states on a bool parameter")
{
    Skeleton skel{1};
    skel.Bones()[0].index = 0;
    skel.Bones()[0].parentIndex = -1;
    skel.FindRootBones();
    skel.BuildChildIndices();
    skel.ComputeInverseBindPoses();

    // Two clips so states have non-zero duration (the player advances normalized time by dt/duration).
    AnimationClip idleClip{u8"idle", 1.0f, true};
    idleClip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
    idleClip.GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 0, 0});
    AnimationClip walkClip{u8"walk", 1.0f, true};
    walkClip.GetOrCreatePositionTrack(0)->AddKeyframe(0.0f, Float3{0, 0, 0});
    walkClip.GetOrCreatePositionTrack(0)->AddKeyframe(1.0f, Float3{0, 0, 0});

    AnimationGraph graph;
    const i32 pMoving = graph.AddParameter(u8"Moving", AnimationParameterType::Bool);
    auto layer = MakeUnique<AnimationLayer>(DefaultAllocator(), StringView{u8"Base"});
    layer->AddState(MakeUnique<AnimationGraphState>(
        DefaultAllocator(), StringView{u8"Idle"},
        MakeUnique<ClipStateNode>(DefaultAllocator(), &idleClip))); // state 0
    layer->AddState(MakeUnique<AnimationGraphState>(
        DefaultAllocator(), StringView{u8"Walk"},
        MakeUnique<ClipStateNode>(DefaultAllocator(), &walkClip))); // state 1
    auto toWalk = MakeUnique<AnimationGraphTransition>(DefaultAllocator());
    toWalk->sourceStateIndex = 0;
    toWalk->destStateIndex = 1;
    toWalk->duration = 0.001f;
    toWalk->AddBoolCondition(pMoving, true);
    layer->AddTransition(static_cast<UniquePtr<AnimationGraphTransition>&&>(toWalk));
    graph.AddLayer(static_cast<UniquePtr<AnimationLayer>&&>(layer));

    AnimationGraphPlayer player{graph, skel};
    CHECK(player.GetCurrentStateIndex() == 0); // starts at default (Idle)

    player.Update(0.016f);
    CHECK(player.GetCurrentStateIndex() == 0); // condition false -> stays

    player.SetBool(pMoving, true);
    player.Update(0.016f);
    CHECK(player.GetCurrentStateIndex() == 1); // transitioned to Walk
    Span<const Float4x4> skin = player.GetSkinningMatrices();
    CHECK(skin.Size() == 1);
}
