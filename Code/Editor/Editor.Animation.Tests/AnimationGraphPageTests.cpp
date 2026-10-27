// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The animation graph page over its markup (Data/Editor/AnimationGraphPage.gml) and a scratch
// project, without a host (no preview): the layers and parameters as lists, the chosen layer on the
// graph view, the grid showing what is chosen (a layer, a parameter, a state, a transition) with
// each row where what it is uses it, each edit one undo step; a parameter added by its type and
// removed, a layer added and removed; the preview's parameters a section of the page's own, never
// the graph's. And the graph's data as the page writes it: the new graph's seed, and the asset's
// round trip through the serializer its undo snapshots ride.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.content;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.animation;

using namespace foundation::core;
using namespace pipeline;
using namespace editor;
namespace anim = foundation::animation;
namespace gui = foundation::gui;

TEST_CASE("graph data: the new graph's seed is one layer, an Idle state it starts in, a Speed parameter")
{
    pipeline::AnimationGraphAsset asset;
    pipeline::SeedDefaultAnimationGraph(asset);

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

TEST_CASE("graph data: the asset round-trips its source and its layout (the undo's snapshot path)")
{
    pipeline::AnimationGraphAsset a;
    pipeline::SeedDefaultAnimationGraph(a);
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
    pipeline::AnimationGraphAsset b;
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

namespace
{
    RefPtr<gui::Node> LoadMarkup(StringView relative)
    {
        const String path = PathJoin(PathJoin(StringView(reinterpret_cast<const char8_t*>(RAPTOR_SOURCE_DIR)), u8"Data").AsView(), relative);
        Result<Array<byte>> bytes = ReadFile(path.AsView());
        REQUIRE(bytes.HasValue());
        gui::RegisterGuiElements();
        gui::RegisterToolkitElements();
        static const gui::ElementSet elements = gui::ElementSet::All();
        gui::MarkupLoader loader(elements, DefaultAllocator());
        return loader.LoadFromString(StringView(reinterpret_cast<const utf8char*>(bytes.Value().Data()), bytes.Value().Size()));
    }

    gui::ListEdit ListEditOf(gui::ListEdit::Op kind, usize index)
    {
        gui::ListEdit edit;
        edit.Kind = kind;
        edit.Index = index;
        return edit;
    }

    /// A scratch project holding one graph: the seed (Base, Idle, Speed) and a Run state.
    struct Bed
    {
        String dir = String(u8"scratch_graph_page_project");
        UniquePtr<EditorProject> project;
        EditorContext context{DefaultAllocator()};
        UniquePtr<AnimationGraphEditorPage> page;

        Bed()
        {
            pipeline::RegisterAnimationAssets();
            (void)RemoveDirectoryRecursive(dir.AsView());
            REQUIRE(EditorProject::Create(DefaultAllocator(), dir.AsView(), u8"P").IsOk());
            project = EditorProject::Open(DefaultAllocator(), dir.AsView());
            REQUIRE(static_cast<bool>(project));
            context.SetProject(project.Get());
            foundation::content::Instance* instance = project->SourceDb().RootGroup()->CreateInstance(u8"Hero", pipeline::AnimationGraphAsset::StaticType());
            REQUIRE(instance != nullptr);
            pipeline::AnimationGraphAsset asset;
            pipeline::SeedDefaultAnimationGraph(asset);
            (void)animgraph::AddState(asset, 0, u8"Run", animgraph::StateKind::Clip, Float2{500.0f, 120.0f});
            REQUIRE(instance->WriteObject(asset).IsOk());
            app::AssetPageServices services;
            services.loadMarkup = [](StringView path) { return LoadMarkup(path); };
            page = MakeUnique<AnimationGraphEditorPage>(DefaultAllocator(), context, Move(services), *instance);
            REQUIRE_FALSE(page->LoadFailed());
        }
        ~Bed()
        {
            page.Reset();
            context.SetProject(nullptr);
            project.Reset();
            (void)RemoveDirectoryRecursive(dir.AsView());
        }
        anim::AnimationGraphSource& Source() { return page->Graph()->source; }
        static void Edit(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
        {
            rows.BeginEdit(path);
            rows.CommitEdit(path, gui::PropertyWrite{value});
        }
        static Optional<gui::PropertyInfo> Info(gui::PropertyBag& rows, StringView parent, StringView name)
        {
            Array<gui::PropertyInfo> found;
            rows.GetProperties(parent, found);
            for (gui::PropertyInfo& info : found)
                if (info.Name == name)
                    return Optional<gui::PropertyInfo>(Move(info));
            return {};
        }
    };
}

TEST_CASE("graph page: the layers and parameters as lists, the chosen layer on the view, a layer's rows")
{
    Bed bed;
    CHECK(bed.page->LayerList() == String(u8"Base"));
    CHECK(bed.page->ParameterList() == String(u8"Speed  Float"));
    CHECK(bed.page->Fact(u8"States") == String(u8"2"));
    CHECK(bed.page->Chosen().kind == GraphChoiceKind::Layer);
    Array<gui::GraphId> nodes;
    bed.page->States().GetNodes(nodes);
    CHECK(nodes.Size() == 3u); // Any State, Idle, Run

    CHECK(bed.page->LayerRows().GetValue(u8"name", 0).AsString() == StringView(u8"Base"));
    const Optional<gui::PropertyInfo> start = Bed::Info(bed.page->LayerRows(), StringView{}, u8"start");
    REQUIRE(start.HasValue());
    REQUIRE(start->Choices.Size() == 2u); // the layer's states by name
    CHECK(start->Choices[1].Name == String(u8"Run"));
    Bed::Edit(bed.page->LayerRows(), u8"start", gui::Variant(static_cast<i64>(1)));
    CHECK(bed.Source().layers[0].defaultState == 1);
    bed.page->Commands().Undo();
    CHECK(bed.Source().layers[0].defaultState == 0);
}

TEST_CASE("graph page: a state's rows shown where what it plays uses them; its entries a list; one step each")
{
    Bed bed;
    bed.page->Choose(GraphChoice{GraphChoiceKind::State, 1});
    CHECK(bed.page->StateRows().GetValue(u8"name", 0).AsString() == StringView(u8"Run"));
    CHECK(Bed::Info(bed.page->StateRows(), StringView{}, u8"clip")->Visible);
    CHECK_FALSE(Bed::Info(bed.page->StateRows(), StringView{}, u8"driver")->Visible);
    CHECK_FALSE(Bed::Info(bed.page->StateRows(), StringView{}, u8"entries")->Visible);

    // A blend by one parameter: its driver and entries, each at a threshold.
    Bed::Edit(bed.page->StateRows(), u8"kind", gui::Variant(static_cast<i64>(1)));
    CHECK(bed.Source().layers[0].states[1].node.kind == 1);
    CHECK_FALSE(Bed::Info(bed.page->StateRows(), StringView{}, u8"clip")->Visible);
    const Optional<gui::PropertyInfo> driver = Bed::Info(bed.page->StateRows(), StringView{}, u8"driver");
    REQUIRE(driver.HasValue());
    CHECK(driver->Visible);
    REQUIRE(driver->Choices.Size() == 2u); // Nothing, Speed
    CHECK(driver->Choices[1].Name == String(u8"Speed"));
    Bed::Edit(bed.page->StateRows(), u8"driver", gui::Variant(static_cast<i64>(0)));
    REQUIRE(bed.page->StateRows().EditList(u8"entries", ListEditOf(gui::ListEdit::Op::Insert, 0)));
    REQUIRE(bed.page->StateRows().EditList(u8"entries", ListEditOf(gui::ListEdit::Op::Insert, 1)));
    const anim::GraphNodeData& node = bed.Source().layers[0].states[1].node;
    REQUIRE(node.entryClips.Size() == 2u);
    CHECK(node.entryThresholds[1] == doctest::Approx(1.0f)); // a step past the last
    Bed::Edit(bed.page->StateRows(), u8"entries.1.threshold", gui::Variant(4.0));
    CHECK(bed.Source().layers[0].states[1].node.entryThresholds[1] == doctest::Approx(4.0f));
    CHECK(Bed::Info(bed.page->StateRows(), u8"entries.0", u8"threshold")->Visible);
    CHECK_FALSE(Bed::Info(bed.page->StateRows(), u8"entries.0", u8"x")->Visible);

    for (int i = 0; i < 5; ++i)
        bed.page->Commands().Undo();
    CHECK(bed.Source().layers[0].states[1].node.kind == 0);
    CHECK(bed.Source().layers[0].states[1].node.entryClips.IsEmpty());
}

TEST_CASE("graph page: a transition made on the view is one step; its conditions a list read as what they say")
{
    Bed bed;
    gui::GraphLinkInfo link;
    link.Kind = gui::GraphLinkKind::Nodes;
    link.FromNode = bed.page->States().StateId(0);
    link.ToNode = bed.page->States().StateId(1);
    const gui::GraphId made = bed.page->States().Connect(link);
    REQUIRE(made != 0);
    REQUIRE(bed.Source().layers[0].transitions.Size() == 1u);
    CHECK(bed.page->Commands().CanUndo());

    bed.page->Choose(GraphChoice{GraphChoiceKind::Transition, 0});
    CHECK(bed.page->TransitionRows().GetValue(u8"from", 0).AsString() == StringView(u8"Idle"));
    CHECK(bed.page->TransitionRows().GetValue(u8"to", 0).AsString() == StringView(u8"Run"));
    CHECK_FALSE(Bed::Info(bed.page->TransitionRows(), StringView{}, u8"exitTime")->Visible);
    Bed::Edit(bed.page->TransitionRows(), u8"exit", gui::Variant(true));
    CHECK(Bed::Info(bed.page->TransitionRows(), StringView{}, u8"exitTime")->Visible);
    REQUIRE(bed.page->TransitionRows().EditList(u8"conditions", ListEditOf(gui::ListEdit::Op::Insert, 0)));
    Bed::Edit(bed.page->TransitionRows(), u8"conditions.0.compare", gui::Variant(static_cast<i64>(2)));
    Bed::Edit(bed.page->TransitionRows(), u8"conditions.0.value", gui::Variant(0.5));
    const anim::GraphConditionData& condition = bed.Source().layers[0].transitions[0].conditions[0];
    CHECK(condition.paramIndex == 0);
    CHECK(condition.op == 2);
    CHECK(condition.threshold == doctest::Approx(0.5f));
    const Optional<gui::PropertyInfo> closed = Bed::Info(bed.page->TransitionRows(), u8"conditions", u8"0");
    REQUIRE(closed.HasValue());
    CHECK(closed->Summary == String(u8"Speed is above 0.5"));

    for (int i = 0; i < 4; ++i)
        bed.page->Commands().Undo();
    CHECK(bed.Source().layers[0].transitions[0].conditions.IsEmpty());
    bed.page->Commands().Undo(); // the transition itself
    CHECK(bed.Source().layers[0].transitions.IsEmpty());
    CHECK(bed.page->Chosen().kind == GraphChoiceKind::Layer); // what was chosen went with it
}

TEST_CASE("graph page: parameters added by type and removed, layers added and removed; the preview's parameters its own")
{
    Bed bed;
    bed.page->AddParameter(anim::AnimationParameterType::Trigger);
    CHECK(bed.page->ParameterList() == String(u8"Speed  Float\nTrigger  Trigger"));
    CHECK(bed.page->Chosen().kind == GraphChoiceKind::Parameter);
    CHECK(bed.page->Chosen().index == 1);
    CHECK_FALSE(Bed::Info(bed.page->ParameterRows(), StringView{}, u8"float")->Visible); // a trigger has no default
    const Optional<gui::PropertyInfo> fire = Bed::Info(bed.page->LiveRows(), StringView{}, u8"p1");
    REQUIRE(fire.HasValue());
    CHECK(fire->DisplayName == String(u8"Fire Trigger"));

    // The preview's values are never the graph's.
    const usize steps = bed.page->Commands().UndoIndex() + 1;
    bed.page->SetLive(0, 0.75);
    Bed::Edit(bed.page->LiveRows(), u8"p0", gui::Variant(0.5));
    CHECK(bed.Source().paramFloats[0] == 0.0f);
    CHECK(static_cast<usize>(bed.page->Commands().UndoIndex() + 1) == steps);

    bed.page->Choose(GraphChoice{GraphChoiceKind::Parameter, 0});
    CHECK(bed.page->RemoveParameter());
    CHECK(bed.page->ParameterList() == String(u8"Trigger  Trigger"));
    CHECK_FALSE(Bed::Info(bed.page->LiveRows(), StringView{}, u8"p1").HasValue()); // made again for one parameter

    bed.page->AddLayer();
    CHECK(bed.page->LayerList() == String(u8"Base\nLayer"));
    CHECK(bed.page->Layer() == 1);
    CHECK(bed.page->RemoveLayer());
    CHECK(bed.page->LayerList() == String(u8"Base"));
    CHECK_FALSE(bed.page->RemoveLayer()); // the last stays
}
