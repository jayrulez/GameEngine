// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:graph_page` partition (implementation).

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

module editor.animation;

import foundation.core;
import foundation.settings; // per-project editor settings, which keep the preview's rig per graph
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.resource;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace animation = foundation::animation;

    // The preview's rig per graph ({graph -> skeleton, skinned mesh}), a section of the per-project
    // editor settings. Its type id must not change, or settings already saved under it stop reading.
    struct GraphPreviewPref
    {
        Guid asset;
        Guid skeleton;
        Guid mesh;
        void Serialize(ISerializer& ar)
        {
            ar.Key("asset");
            ar.GuidValue(asset);
            ar.Key("skeleton");
            ar.GuidValue(skeleton);
            ar.Key("mesh");
            ar.GuidValue(mesh);
        }
    };
    inline void Serialize(ISerializer& ar, GraphPreviewPref& p)
    {
        ar.BeginObject();
        p.Serialize(ar);
        ar.EndObject();
    }
    class GraphPreviewSettings final : public ISerializable
    {
        RTTI_OBJECT(GraphPreviewSettings, ISerializable)
    public:
        Array<GraphPreviewPref> prefs;
        void Serialize(ISerializer& ar) override { foundation::core::Serialize(ar, "prefs", prefs); }
    };

    namespace
    {
        constexpr StringView kEntries = u8"entries";
        constexpr StringView kConditions = u8"conditions";
        constexpr StringView kParameterTypes[] = {u8"Float", u8"Int", u8"Bool", u8"Trigger"};
        constexpr StringView kBlendModes[] = {u8"Override", u8"Additive"};
        constexpr StringView kStateKinds[] = {u8"A clip", u8"A blend by one parameter", u8"A blend by two parameters"};
        constexpr StringView kComparisons[] = {u8"equals", u8"differs from", u8"is above", u8"is below", u8"is at least", u8"is at most"};

        [[nodiscard]] gui::Variant GuidText(const Guid& id)
        {
            if (id.IsNil())
                return gui::Variant(StringView{});
            utf8char text[37];
            id.ToChars(text);
            return gui::Variant(String(StringView(text, 36)));
        }
        [[nodiscard]] Guid GuidOf(const gui::Variant& value)
        {
            Guid id;
            const StringView text = Trim(value.AsString());
            if (!text.IsEmpty())
                (void)Guid::TryParse(text, id);
            return id;
        }

        [[nodiscard]] StringView LastSegment(StringView path)
        {
            for (usize i = path.Size(); i-- > 0;)
                if (path[i] == u8'.')
                    return path.SubStr(i + 1, path.Size() - i - 1);
            return path;
        }
        // The index of the element of `list` that a path is in ("entries.2.clip" in "entries" gives
        // 2), or -1.
        [[nodiscard]] i32 ElementOf(StringView path, StringView list)
        {
            if (!path.StartsWith(list) || path.Size() <= list.Size() + 1 || path[list.Size()] != u8'.')
                return -1;
            const StringView rest = path.SubStr(list.Size() + 1, path.Size() - list.Size() - 1);
            usize dot = 0;
            while (dot < rest.Size() && rest[dot] != u8'.')
                ++dot;
            const Optional<i64> index = ParseInt(rest.SubStr(0, dot));
            return index.HasValue() && index.Value() >= 0 ? static_cast<i32>(index.Value()) : -1;
        }

        gui::PropertyInfo& Row(gui::PropertyBag& rows, StringView path, gui::PropertyKind kind, const gui::Variant& value, StringView shown, StringView about)
        {
            gui::PropertyInfo& info = rows.Add(path, kind, value);
            info.DisplayName = String(shown);
            info.Description = String(about);
            return info;
        }
        void Choices(gui::PropertyInfo& info, Span<const StringView> names)
        {
            info.Choices.Clear();
            for (usize i = 0; i < names.Size(); ++i)
                info.Choices.PushBack(gui::PropertyChoice{String(names[i]), static_cast<i64>(i)});
        }

        // Makes the graph's parallel parameter arrays as long as its names.
        void SquareParameters(animation::AnimationGraphSource& source)
        {
            const usize count = source.paramNames.Size();
            source.paramTypes.Resize(count, 0);
            source.paramFloats.Resize(count, 0.0f);
            source.paramInts.Resize(count, 0);
            source.paramBools.Resize(count, 0);
        }
        // Makes each of a blend's entry arrays as long as its clips.
        void SquareEntries(animation::GraphNodeData& node)
        {
            node.entryThresholds.Resize(node.entryClips.Size(), 0.0f);
            node.entryPositions.Resize(node.entryClips.Size(), Float2{});
        }
    }

    AnimationGraphEditorPage::AnimationGraphEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance)
        : app::AssetObjectPage(context, Move(services), instance, pipeline::AnimationGraphAsset::StaticType(), kPath)
    {
        HideAssetRows(); // the asset keeps parallel arrays and numeric kinds, so the page builds its own rows
        Provide<IPlaybackPage>(*this);
        AnimationGraphEditorPage* self = this;
        m_states = MakeUnique<AnimationGraphStates>(Allocator(), [self]() { return self->Graph(); },
                                                    [self](StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate) { self->Edit(typeId, mutate); });
        m_states->NameOf = [self](const Guid& id) { return self->ClipName(id); };
        m_states->Refresh(); // so the states show their clips' names
        BuildRows();

        m_layerModel = MakeUnique<gui::TreeModel>(Allocator());
        m_parameterModel = MakeUnique<gui::TreeModel>(Allocator());
        m_layerTree = Find<gui::TreeView>(u8"graph-layers");
        if (m_layerTree != nullptr)
        {
            m_layerTree->SetModel(m_layerModel.Get());
            m_layerTree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                               {
                                                   if (!self->m_showing && index.IsValid())
                                                       self->ChooseLayer(static_cast<i32>(index.InternalId));
                                               });
        }
        m_parameterTree = Find<gui::TreeView>(u8"graph-parameters");
        if (m_parameterTree != nullptr)
        {
            m_parameterTree->SetModel(m_parameterModel.Get());
            m_parameterTree->SetOnSelectionChanged([self](gui::ModelIndex index)
                                                   {
                                                       if (!self->m_showing && index.IsValid())
                                                           self->Choose(GraphChoice{GraphChoiceKind::Parameter, static_cast<i32>(index.InternalId)});
                                                   });
        }
        // A row's context menu chooses the row, then offers Delete (as the bar's trash button does).
        const auto rowMenu = [self](gui::TreeView* tree, bool layers)
        {
            if (tree == nullptr)
                return;
            (void)tree->AddEventListener(gui::EventType::ContextMenu, [self, tree, layers](const gui::Event& e)
                                         {
                                             const Float2 at = static_cast<const gui::MouseEvent&>(e).Position;
                                             const i32 item = tree->ItemAt(at);
                                             if (item < 0)
                                                 return;
                                             self->ShowListMenu(layers, static_cast<i32>(tree->GetItemIndex(item).InternalId), *tree, at);
                                             e.Handled = true;
                                         });
        };
        rowMenu(m_layerTree, true);
        rowMenu(m_parameterTree, false);
        if (auto* add = Find<gui::ButtonBase>(u8"graph-add-layer"))
            add->SetOnClick([self]() { self->AddLayer(); });
        if (auto* remove = Find<gui::ButtonBase>(u8"graph-remove-layer"))
            remove->SetOnClick([self]() { (void)self->RemoveLayer(); });
        if (auto* add = Find<gui::ToolMenuButton>(u8"graph-add-parameter"))
            for (usize t = 0; t < 4; ++t)
            {
                const auto type = static_cast<animation::AnimationParameterType>(t);
                (void)add->GetMenu().AddItem(kParameterTypes[t], [self, type]() { self->AddParameter(type); });
            }
        if (auto* remove = Find<gui::ButtonBase>(u8"graph-remove-parameter"))
            remove->SetOnClick([self]() { (void)self->RemoveParameter(); });

        m_view = Find<gui::GraphView>(u8"graph-view");
        if (m_view != nullptr)
        {
            m_view->SetSource(m_states.Get());
            m_view->SetOnSelectionChanged([self]() { self->FollowView(); });
        }

        if (Content())
        {
            m_pane = MakeUnique<app::PreviewPane>(Allocator(), *Content(), Services(), u8"animgraph.preview");
            m_pane->OnFrame = [self]() { self->FramePreview(); };
            m_rig = MakeUnique<AnimationPreviewRig>(Allocator(), Context(), Services(), *m_pane);
            m_rig->OnPicked = [self]()
            {
                self->SavePreviewPref();
                self->m_rebuild = true;
            };
            m_rig->OnFirstShown = [self]() { self->FramePreview(); };
            BindAction(Find<gui::ButtonBase>(u8"graph-restart"), u8"playback.restart");
            BindAction(Find<gui::ButtonBase>(u8"graph-play"), u8"playback.play");
            BindAction(Find<gui::ButtonBase>(u8"graph-stop"), u8"playback.stop");
            m_state = Find<gui::Label>(u8"graph-state");
            m_travelButton = Find<gui::ButtonBase>(u8"graph-travel");
            if (m_travelButton != nullptr)
                m_travelButton->SetOnClick([self]()
                                           {
                                               self->m_travel = !self->m_travel;
                                               self->m_travelButton->SetChecked(self->m_travel);
                                               self->m_travelPosition = Float3{};
                                               self->m_travelYaw = 0.0f;
                                           });
        }
        LoadPreviewPref();
        BuildLiveRows();
        ShowLists();
        Choose(GraphChoice{GraphChoiceKind::Layer, 0});
        ShowFacts();
    }

    AnimationGraphEditorPage::~AnimationGraphEditorPage()
    {
        if (m_view != nullptr)
            m_view->SetSource(nullptr);
        if (m_layerTree != nullptr)
            m_layerTree->SetModel(nullptr);
        if (m_parameterTree != nullptr)
            m_parameterTree->SetModel(nullptr);
        m_player.Reset(); // reset before the graph it borrows
    }

    pipeline::AnimationGraphAsset* AnimationGraphEditorPage::Graph() const
    {
        return ObjectAs<pipeline::AnimationGraphAsset>();
    }

    String AnimationGraphEditorPage::ClipName(const Guid& id) const
    {
        foundation::content::Instance* instance = Context().Project() != nullptr ? Context().Project()->SourceDb().GetInstance(id) : nullptr;
        return instance != nullptr ? String(instance->Name()) : String(u8"A missing clip");
    }

    void AnimationGraphEditorPage::Edit(StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate)
    {
        EditWhole(typeId, [&mutate](ISerializable& object) { mutate(static_cast<pipeline::AnimationGraphAsset&>(object)); });
    }

    // === The rows ===

    void AnimationGraphEditorPage::BuildRows()
    {
        AnimationGraphEditorPage* self = this;
        m_layerRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        Row(*m_layerRows, u8"name", gui::PropertyKind::String, gui::Variant(StringView{}), u8"Name", u8"What the layer is called");
        Choices(Row(*m_layerRows, u8"blend", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(0)), u8"Blend",
                    u8"Over the layers under it (Override), or added to them (Additive)"),
                Span<const StringView>(kBlendModes, 2u));
        gui::PropertyInfo& weight = Row(*m_layerRows, u8"weight", gui::PropertyKind::Float, gui::Variant(1.0), u8"Weight", u8"How much of it shows over the layers under it");
        weight.HasRange = true;
        weight.Min = 0.0;
        weight.Max = 1.0;
        weight.Step = 0.01;
        weight.Hint = String(u8"slider");
        Row(*m_layerRows, u8"start", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(0)), u8"Starts in", u8"The state the layer plays first");
        m_layerRows->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                   {
                                       if (path != StringView(u8"start"))
                                           return;
                                       info.Choices.Clear();
                                       pipeline::AnimationGraphAsset* graph = self->Graph();
                                       if (graph == nullptr || self->m_layer >= static_cast<i32>(graph->source.layers.Size()))
                                           return;
                                       const auto& states = graph->source.layers[static_cast<usize>(self->m_layer)].states;
                                       for (usize s = 0; s < states.Size(); ++s)
                                           info.Choices.PushBack(gui::PropertyChoice{String(states[s].name.AsView()), static_cast<i64>(s)});
                                   });

        m_parameterRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        Row(*m_parameterRows, u8"name", gui::PropertyKind::String, gui::Variant(StringView{}), u8"Name", u8"What a script sets it by");
        Choices(Row(*m_parameterRows, u8"type", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(0)), u8"Type",
                    u8"A number, a whole number, on or off, or a trigger (on for one step, then off)"),
                Span<const StringView>(kParameterTypes, 4u));
        Row(*m_parameterRows, u8"float", gui::PropertyKind::Float, gui::Variant(0.0), u8"Default", u8"Its value when the graph starts");
        Row(*m_parameterRows, u8"int", gui::PropertyKind::Integer, gui::Variant(static_cast<i64>(0)), u8"Default", u8"Its value when the graph starts");
        Row(*m_parameterRows, u8"bool", gui::PropertyKind::Bool, gui::Variant(false), u8"Default", u8"Its value when the graph starts");
        m_parameterRows->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                       {
                                           // Show only the default for its type; a trigger has none.
                                           const pipeline::AnimationGraphAsset* graph = self->Graph();
                                           const i32 p = self->m_chosen.index;
                                           const u8 type = graph != nullptr && p >= 0 && static_cast<usize>(p) < graph->source.paramTypes.Size() ? graph->source.paramTypes[static_cast<usize>(p)] : 0;
                                           if (path == StringView(u8"float"))
                                               info.Visible = type == 0;
                                           else if (path == StringView(u8"int"))
                                               info.Visible = type == 1;
                                           else if (path == StringView(u8"bool"))
                                               info.Visible = type == 2;
                                       });

        m_stateRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        Row(*m_stateRows, u8"name", gui::PropertyKind::String, gui::Variant(StringView{}), u8"Name", u8"What the state is called");
        gui::PropertyInfo& speed = Row(*m_stateRows, u8"speed", gui::PropertyKind::Float, gui::Variant(1.0), u8"Speed", u8"How fast it plays (1: as authored)");
        speed.Step = 0.05;
        Row(*m_stateRows, u8"loop", gui::PropertyKind::Bool, gui::Variant(true), u8"Loops", u8"Whether it plays again from its start when it ends; else it holds its last pose");
        Choices(Row(*m_stateRows, u8"kind", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(0)), u8"Plays",
                    u8"One clip, or a blend of clips by one parameter or by two"),
                Span<const StringView>(kStateKinds, 3u));
        gui::PropertyInfo& clip = Row(*m_stateRows, u8"clip", gui::PropertyKind::Reference, gui::Variant(StringView{}), u8"Clip", u8"The clip it plays");
        clip.ReferenceType = &animation::AnimationClip::StaticType();
        Row(*m_stateRows, u8"driver", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(-1)), u8"By", u8"The parameter the blend follows");
        Row(*m_stateRows, u8"driverX", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(-1)), u8"Across by", u8"The parameter across the blend's plane");
        Row(*m_stateRows, u8"driverY", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(-1)), u8"Up by", u8"The parameter up the blend's plane");
        gui::PropertyInfo& entries = m_stateRows->AddList(kEntries, gui::PropertyKind::Group, gui::Variant{});
        entries.DisplayName = String(u8"Entries");
        entries.Description = String(u8"The clips it blends between, each where it plays alone");
        entries.ListOps = gui::PropertyListAdd | gui::PropertyListRemove;
        if (gui::PropertyInfo* field = m_stateRows->AddElementField(kEntries, u8"clip", gui::PropertyKind::Reference, gui::Variant(StringView{})))
        {
            field->DisplayName = String(u8"Clip");
            field->ReferenceType = &animation::AnimationClip::StaticType();
        }
        if (gui::PropertyInfo* field = m_stateRows->AddElementField(kEntries, u8"threshold", gui::PropertyKind::Float, gui::Variant(0.0)))
        {
            field->DisplayName = String(u8"At");
            field->Description = String(u8"The parameter's value where this clip plays alone");
        }
        if (gui::PropertyInfo* field = m_stateRows->AddElementField(kEntries, u8"x", gui::PropertyKind::Float, gui::Variant(0.0)))
            field->DisplayName = String(u8"Across");
        if (gui::PropertyInfo* field = m_stateRows->AddElementField(kEntries, u8"y", gui::PropertyKind::Float, gui::Variant(0.0)))
            field->DisplayName = String(u8"Up");
        m_stateRows->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                   {
                                       const pipeline::AnimationGraphAsset* graph = self->Graph();
                                       const animation::GraphStateData* state = nullptr;
                                       if (graph != nullptr && self->m_layer < static_cast<i32>(graph->source.layers.Size()))
                                       {
                                           const auto& states = graph->source.layers[static_cast<usize>(self->m_layer)].states;
                                           if (self->m_chosen.index >= 0 && static_cast<usize>(self->m_chosen.index) < states.Size())
                                               state = &states[static_cast<usize>(self->m_chosen.index)];
                                       }
                                       const u8 kind = state != nullptr ? state->node.kind : 0;
                                       // Show each row only where the state's kind of playback uses it.
                                       if (path == StringView(u8"clip"))
                                           info.Visible = kind == 0;
                                       else if (path == StringView(u8"driver"))
                                           info.Visible = kind == 1;
                                       else if (path == StringView(u8"driverX") || path == StringView(u8"driverY"))
                                           info.Visible = kind == 2;
                                       else if (path == kEntries)
                                           info.Visible = kind != 0;
                                       if (path.StartsWith(u8"driver") && graph != nullptr)
                                       {
                                           info.Choices.Clear();
                                           info.Choices.PushBack(gui::PropertyChoice{String(u8"Nothing"), -1});
                                           for (usize p = 0; p < graph->source.paramNames.Size(); ++p)
                                               info.Choices.PushBack(gui::PropertyChoice{String(graph->source.paramNames[p].AsView()), static_cast<i64>(p)});
                                       }
                                       const i32 entry = ElementOf(path, kEntries);
                                       if (entry < 0 || state == nullptr)
                                           return;
                                       const StringView field = LastSegment(path);
                                       if (field == StringView(u8"threshold"))
                                           info.Visible = kind == 1;
                                       else if (field == StringView(u8"x") || field == StringView(u8"y"))
                                           info.Visible = kind == 2;
                                       else if (gui::ParentPropertyPath(path) == kEntries && static_cast<usize>(entry) < state->node.entryClips.Size())
                                       {
                                           // While closed, an entry shows its clip, and in a 1D blend its threshold.
                                           const Guid& id = state->node.entryClips[static_cast<usize>(entry)];
                                           info.DisplayName = id.IsNil() ? Format(u8"Entry {}", entry + 1) : self->ClipName(id);
                                           if (kind == 1 && static_cast<usize>(entry) < state->node.entryThresholds.Size())
                                               info.Summary = Format(u8"at {}", state->node.entryThresholds[static_cast<usize>(entry)]);
                                       }
                                   });

        m_transitionRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        Row(*m_transitionRows, u8"from", gui::PropertyKind::String, gui::Variant(StringView{}), u8"From", u8"The state it leaves (Any State: every state)").ReadOnly = true;
        Row(*m_transitionRows, u8"to", gui::PropertyKind::String, gui::Variant(StringView{}), u8"To", u8"The state it goes to").ReadOnly = true;
        gui::PropertyInfo& duration = Row(*m_transitionRows, u8"duration", gui::PropertyKind::Float, gui::Variant(0.25), u8"Blend time",
                                          u8"How long the two states are blended across, in seconds");
        duration.Step = 0.05;
        Row(*m_transitionRows, u8"exit", gui::PropertyKind::Bool, gui::Variant(false), u8"Waits to exit",
            u8"Whether it waits for the state to play so far before it may go (with its conditions, if any)");
        gui::PropertyInfo& exitTime = Row(*m_transitionRows, u8"exitTime", gui::PropertyKind::Float, gui::Variant(1.0), u8"Exits at",
                                          u8"How far through the state it may go, from 0 (its start) to 1 (its end)");
        exitTime.HasRange = true;
        exitTime.Min = 0.0;
        exitTime.Max = 1.0;
        exitTime.Step = 0.01;
        exitTime.Hint = String(u8"slider");
        Row(*m_transitionRows, u8"priority", gui::PropertyKind::Integer, gui::Variant(static_cast<i64>(0)), u8"Priority",
            u8"Which goes when several could: the lowest first");
        gui::PropertyInfo& conditions = m_transitionRows->AddList(kConditions, gui::PropertyKind::Group, gui::Variant{});
        conditions.DisplayName = String(u8"Conditions");
        conditions.Description = String(u8"What must all hold for it to go (none: it goes as soon as it may)");
        conditions.ListOps = gui::PropertyListAdd | gui::PropertyListRemove;
        if (gui::PropertyInfo* field = m_transitionRows->AddElementField(kConditions, u8"parameter", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(0))))
            field->DisplayName = String(u8"Parameter");
        if (gui::PropertyInfo* field = m_transitionRows->AddElementField(kConditions, u8"compare", gui::PropertyKind::Enum, gui::Variant(static_cast<i64>(0))))
        {
            field->DisplayName = String(u8"Holds when it");
            Choices(*field, Span<const StringView>(kComparisons, 6u));
        }
        if (gui::PropertyInfo* field = m_transitionRows->AddElementField(kConditions, u8"value", gui::PropertyKind::Float, gui::Variant(1.0)))
        {
            field->DisplayName = String(u8"Value");
            field->Description = String(u8"What it is compared with (on and off: 1 and 0)");
        }
        m_transitionRows->SetOnDescribe([self](StringView path, gui::PropertyInfo& info)
                                        {
                                            const pipeline::AnimationGraphAsset* graph = self->Graph();
                                            if (graph == nullptr)
                                                return;
                                            if (path == StringView(u8"exitTime"))
                                            {
                                                info.Visible = self->m_transitionRows->GetValue(u8"exit", 0).AsBool();
                                                return;
                                            }
                                            const i32 condition = ElementOf(path, kConditions);
                                            if (condition < 0)
                                                return;
                                            const auto& source = graph->source;
                                            const auto name = [&source](i32 p)
                                            {
                                                return p >= 0 && static_cast<usize>(p) < source.paramNames.Size() ? String(source.paramNames[static_cast<usize>(p)].AsView())
                                                                                                                  : String(u8"Nothing");
                                            };
                                            if (LastSegment(path) == StringView(u8"parameter"))
                                            {
                                                info.Choices.Clear();
                                                for (usize p = 0; p < source.paramNames.Size(); ++p)
                                                    info.Choices.PushBack(gui::PropertyChoice{String(source.paramNames[p].AsView()), static_cast<i64>(p)});
                                                return;
                                            }
                                            if (gui::ParentPropertyPath(path) != kConditions)
                                                return;
                                            // While closed, a condition reads as a sentence ("Speed is above 0.5").
                                            const i64 p = self->m_transitionRows->GetValue(gui::JoinPropertyPath(path, u8"parameter").AsView(), 0).AsInt();
                                            const i64 op = Clamp<i64>(self->m_transitionRows->GetValue(gui::JoinPropertyPath(path, u8"compare").AsView(), 0).AsInt(), 0, 5);
                                            const f64 value = self->m_transitionRows->GetValue(gui::JoinPropertyPath(path, u8"value").AsView(), 0).AsFloat();
                                            info.DisplayName = Format(u8"Condition {}", condition + 1);
                                            info.Summary = Format(u8"{} {} {}", name(static_cast<i32>(p)), kComparisons[op], value);
                                        });

        gui::PropertyBag* const bags[] = {m_layerRows.Get(), m_parameterRows.Get(), m_stateRows.Get(), m_transitionRows.Get()};
        for (gui::PropertyBag* raw : bags)
        {
            gui::PropertyBag* rows = raw;
            rows->SetOnCommit([self, rows](const gui::PropertyBag::Commit& commit)
                              {
                                  if (!commit.After.IsEmpty())
                                      self->Committed(*rows, commit.Path.AsView(), commit.After[0]);
                              });
            rows->SetOnListEdited([self, rows](StringView path, const gui::ListEdit& edit) { self->ListEdited(*rows, path, edit); });
        }
    }

    void AnimationGraphEditorPage::BuildLiveRows()
    {
        // Rebuilt whenever the parameters change: one row per parameter, edited by its type.
        const pipeline::AnimationGraphAsset* graph = Graph();
        const usize count = graph != nullptr ? graph->source.paramNames.Size() : 0;
        if (count == m_liveCount && m_liveRows)
            return;
        m_liveCount = count;
        ClearSections(); // the grid releases the old rows before they are destroyed
        m_liveRows = MakeUnique<gui::PropertyBag>(Allocator(), Allocator());
        m_live.Resize(count, 0.0);
        AnimationGraphEditorPage* self = this;
        for (usize p = 0; p < count; ++p)
        {
            const auto& source = graph->source;
            const u8 type = p < source.paramTypes.Size() ? source.paramTypes[p] : 0;
            m_live[p] = type == 0 ? static_cast<f64>(p < source.paramFloats.Size() ? source.paramFloats[p] : 0.0f)
                      : type == 1 ? static_cast<f64>(p < source.paramInts.Size() ? source.paramInts[p] : 0)
                      : type == 2 ? (p < source.paramBools.Size() && source.paramBools[p] != 0 ? 1.0 : 0.0)
                                  : 0.0;
            const String path = Format(u8"p{}", p);
            const gui::PropertyKind kind = type == 0 ? gui::PropertyKind::Float : type == 1 ? gui::PropertyKind::Integer : gui::PropertyKind::Bool;
            const gui::Variant value = type == 0 ? gui::Variant(m_live[p]) : type == 1 ? gui::Variant(static_cast<i64>(m_live[p])) : gui::Variant(m_live[p] != 0.0);
            gui::PropertyInfo& info = m_liveRows->Add(path.AsView(), kind, value);
            info.DisplayName = type == 3 ? Format(u8"Fire {}", source.paramNames[p]) : String(source.paramNames[p].AsView());
            info.Description = type == 3 ? String(u8"Ticked, the trigger fires once in the preview") : String(u8"Its value in the preview: this page's alone, never the graph's");
            if (type == 0)
                info.Step = 0.05;
        }
        m_liveRows->SetOnCommit([self](const gui::PropertyBag::Commit& commit)
                                {
                                    if (!commit.After.IsEmpty())
                                        self->LiveCommitted(commit.Path.AsView(), commit.After[0]);
                                });
        ShowSections();
    }

    void AnimationGraphEditorPage::ShowSections()
    {
        ClearSections();
        switch (m_chosen.kind)
        {
        case GraphChoiceKind::Layer:
            AddSection(u8"layer", u8"Layer", *m_layerRows);
            break;
        case GraphChoiceKind::Parameter:
            AddSection(u8"parameter", u8"Parameter", *m_parameterRows);
            break;
        case GraphChoiceKind::State:
            AddSection(u8"state", u8"State", *m_stateRows);
            break;
        case GraphChoiceKind::Transition:
            AddSection(u8"transition", u8"Transition", *m_transitionRows);
            break;
        }
        if (m_liveRows)
            AddSection(u8"preview", u8"Preview parameters", *m_liveRows);
    }

    void AnimationGraphEditorPage::ShowRows()
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr || graph->source.layers.IsEmpty())
            return;
        const auto& source = graph->source;
        const animation::GraphLayerData& layer = source.layers[static_cast<usize>(m_layer)];
        switch (m_chosen.kind)
        {
        case GraphChoiceKind::Layer:
            m_layerRows->SetValue(u8"name", 0, gui::Variant(layer.name.AsView()));
            m_layerRows->SetValue(u8"blend", 0, gui::Variant(static_cast<i64>(layer.blendMode)));
            m_layerRows->SetValue(u8"weight", 0, gui::Variant(static_cast<f64>(layer.weight)));
            m_layerRows->SetValue(u8"start", 0, gui::Variant(static_cast<i64>(layer.defaultState)));
            m_layerRows->PropertiesChanged();
            break;
        case GraphChoiceKind::Parameter:
        {
            const usize p = static_cast<usize>(m_chosen.index);
            if (p >= source.paramNames.Size())
                return;
            m_parameterRows->SetValue(u8"name", 0, gui::Variant(source.paramNames[p].AsView()));
            m_parameterRows->SetValue(u8"type", 0, gui::Variant(static_cast<i64>(p < source.paramTypes.Size() ? source.paramTypes[p] : 0)));
            m_parameterRows->SetValue(u8"float", 0, gui::Variant(static_cast<f64>(p < source.paramFloats.Size() ? source.paramFloats[p] : 0.0f)));
            m_parameterRows->SetValue(u8"int", 0, gui::Variant(static_cast<i64>(p < source.paramInts.Size() ? source.paramInts[p] : 0)));
            m_parameterRows->SetValue(u8"bool", 0, gui::Variant(p < source.paramBools.Size() && source.paramBools[p] != 0));
            m_parameterRows->PropertiesChanged();
            break;
        }
        case GraphChoiceKind::State:
        {
            const usize s = static_cast<usize>(m_chosen.index);
            if (s >= layer.states.Size())
                return;
            const animation::GraphStateData& state = layer.states[s];
            m_stateRows->SetValue(u8"name", 0, gui::Variant(state.name.AsView()));
            m_stateRows->SetValue(u8"speed", 0, gui::Variant(static_cast<f64>(state.speed)));
            m_stateRows->SetValue(u8"loop", 0, gui::Variant(state.loop));
            m_stateRows->SetValue(u8"kind", 0, gui::Variant(static_cast<i64>(state.node.kind)));
            m_stateRows->SetValue(u8"clip", 0, GuidText(state.node.clipRef));
            m_stateRows->SetValue(u8"driver", 0, gui::Variant(static_cast<i64>(state.node.paramIndex)));
            m_stateRows->SetValue(u8"driverX", 0, gui::Variant(static_cast<i64>(state.node.paramIndexX)));
            m_stateRows->SetValue(u8"driverY", 0, gui::Variant(static_cast<i64>(state.node.paramIndexY)));
            m_stateRows->ResizeList(kEntries, 0, state.node.entryClips.Size());
            for (usize e = 0; e < state.node.entryClips.Size(); ++e)
            {
                const String element = Format(u8"{}.{}", kEntries, e);
                m_stateRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"clip").AsView(), 0, GuidText(state.node.entryClips[e]));
                m_stateRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"threshold").AsView(), 0,
                                      gui::Variant(static_cast<f64>(e < state.node.entryThresholds.Size() ? state.node.entryThresholds[e] : 0.0f)));
                const Float2 at = e < state.node.entryPositions.Size() ? state.node.entryPositions[e] : Float2{};
                m_stateRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"x").AsView(), 0, gui::Variant(static_cast<f64>(at.x)));
                m_stateRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"y").AsView(), 0, gui::Variant(static_cast<f64>(at.y)));
            }
            m_stateRows->PropertiesChanged();
            break;
        }
        case GraphChoiceKind::Transition:
        {
            const usize t = static_cast<usize>(m_chosen.index);
            if (t >= layer.transitions.Size())
                return;
            const animation::GraphTransitionData& transition = layer.transitions[t];
            const auto stateName = [&layer](i32 s)
            {
                return s < 0 ? String(u8"Any State") : s < static_cast<i32>(layer.states.Size()) ? String(layer.states[static_cast<usize>(s)].name.AsView()) : String(u8"Nothing");
            };
            m_transitionRows->SetValue(u8"from", 0, gui::Variant(stateName(transition.src).AsView()));
            m_transitionRows->SetValue(u8"to", 0, gui::Variant(stateName(transition.dst).AsView()));
            m_transitionRows->SetValue(u8"duration", 0, gui::Variant(static_cast<f64>(transition.duration)));
            m_transitionRows->SetValue(u8"exit", 0, gui::Variant(transition.hasExitTime));
            m_transitionRows->SetValue(u8"exitTime", 0, gui::Variant(static_cast<f64>(transition.exitTime)));
            m_transitionRows->SetValue(u8"priority", 0, gui::Variant(static_cast<i64>(transition.priority)));
            m_transitionRows->ResizeList(kConditions, 0, transition.conditions.Size());
            for (usize c = 0; c < transition.conditions.Size(); ++c)
            {
                const String element = Format(u8"{}.{}", kConditions, c);
                const animation::GraphConditionData& condition = transition.conditions[c];
                m_transitionRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"parameter").AsView(), 0, gui::Variant(static_cast<i64>(condition.paramIndex)));
                m_transitionRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"compare").AsView(), 0, gui::Variant(static_cast<i64>(condition.op)));
                m_transitionRows->SetValue(gui::JoinPropertyPath(element.AsView(), u8"value").AsView(), 0, gui::Variant(static_cast<f64>(condition.threshold)));
            }
            m_transitionRows->PropertiesChanged();
            break;
        }
        }
    }

    void AnimationGraphEditorPage::ShowFacts()
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr)
            return;
        usize states = 0;
        usize transitions = 0;
        for (const animation::GraphLayerData& layer : graph->source.layers)
        {
            states += layer.states.Size();
            transitions += layer.transitions.Size();
        }
        SetFact(u8"Layers", Format(u8"{}", graph->source.layers.Size()).AsView());
        SetFact(u8"States", Format(u8"{}", states).AsView());
        SetFact(u8"Transitions", Format(u8"{}", transitions).AsView());
        SetFact(u8"Parameters", Format(u8"{}", graph->source.paramNames.Size()).AsView());
    }

    void AnimationGraphEditorPage::Committed(gui::PropertyBag& rows, StringView path, const gui::Variant& value)
    {
        pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr)
            return;
        const usize l = static_cast<usize>(m_layer);
        const usize at = static_cast<usize>(Max(m_chosen.index, 0));
        if (&rows == m_layerRows.Get())
        {
            const String text(value.AsString());
            const i64 number = value.AsInt();
            const f64 real = value.AsFloat();
            Edit(u8"edit_animgraph_layer", [l, path, &text, number, real](pipeline::AnimationGraphAsset& asset)
                 {
                     animation::GraphLayerData& layer = asset.source.layers[l];
                     if (path == StringView(u8"name"))
                         layer.name = text;
                     else if (path == StringView(u8"blend"))
                         layer.blendMode = static_cast<u8>(Clamp<i64>(number, 0, 1));
                     else if (path == StringView(u8"weight"))
                         layer.weight = static_cast<f32>(Clamp(real, 0.0, 1.0));
                     else if (path == StringView(u8"start"))
                         layer.defaultState = static_cast<i32>(Clamp<i64>(number, 0, static_cast<i64>(layer.states.Size()) - 1));
                 });
            return;
        }
        if (&rows == m_parameterRows.Get())
        {
            if (at >= graph->source.paramNames.Size())
                return;
            const String text(value.AsString());
            const i64 number = value.AsInt();
            const f64 real = value.AsFloat();
            const bool on = value.AsBool();
            Edit(u8"edit_animgraph_parameter", [at, path, &text, number, real, on](pipeline::AnimationGraphAsset& asset)
                 {
                     animation::AnimationGraphSource& source = asset.source;
                     SquareParameters(source);
                     if (path == StringView(u8"name"))
                         source.paramNames[at] = text;
                     else if (path == StringView(u8"type"))
                         source.paramTypes[at] = static_cast<u8>(Clamp<i64>(number, 0, 3));
                     else if (path == StringView(u8"float"))
                         source.paramFloats[at] = static_cast<f32>(real);
                     else if (path == StringView(u8"int"))
                         source.paramInts[at] = static_cast<i32>(number);
                     else if (path == StringView(u8"bool"))
                         source.paramBools[at] = on ? 1 : 0;
                 });
            return;
        }
        if (&rows == m_stateRows.Get())
        {
            if (at >= graph->source.layers[l].states.Size())
                return;
            const String text(value.AsString());
            const i64 number = value.AsInt();
            const f64 real = value.AsFloat();
            const bool on = value.AsBool();
            const Guid id = GuidOf(value);
            const i32 entry = ElementOf(path, kEntries);
            const StringView field = LastSegment(path);
            Edit(u8"edit_animgraph_state", [l, at, path, field, entry, &text, number, real, on, id](pipeline::AnimationGraphAsset& asset)
                 {
                     animation::GraphStateData& state = asset.source.layers[l].states[at];
                     animation::GraphNodeData& node = state.node;
                     if (entry >= 0)
                     {
                         SquareEntries(node);
                         if (static_cast<usize>(entry) >= node.entryClips.Size())
                             return;
                         const usize e = static_cast<usize>(entry);
                         if (field == StringView(u8"clip"))
                             node.entryClips[e] = id;
                         else if (field == StringView(u8"threshold"))
                             node.entryThresholds[e] = static_cast<f32>(real);
                         else if (field == StringView(u8"x"))
                             node.entryPositions[e].x = static_cast<f32>(real);
                         else if (field == StringView(u8"y"))
                             node.entryPositions[e].y = static_cast<f32>(real);
                         return;
                     }
                     if (path == StringView(u8"name"))
                         state.name = text;
                     else if (path == StringView(u8"speed"))
                         state.speed = static_cast<f32>(real);
                     else if (path == StringView(u8"loop"))
                         state.loop = on;
                     else if (path == StringView(u8"kind"))
                         node.kind = static_cast<u8>(Clamp<i64>(number, 0, 2));
                     else if (path == StringView(u8"clip"))
                         node.clipRef = id;
                     else if (path == StringView(u8"driver"))
                         node.paramIndex = static_cast<i32>(number);
                     else if (path == StringView(u8"driverX"))
                         node.paramIndexX = static_cast<i32>(number);
                     else if (path == StringView(u8"driverY"))
                         node.paramIndexY = static_cast<i32>(number);
                 });
            return;
        }
        if (&rows == m_transitionRows.Get())
        {
            if (at >= graph->source.layers[l].transitions.Size())
                return;
            const i64 number = value.AsInt();
            const f64 real = value.AsFloat();
            const bool on = value.AsBool();
            const i32 condition = ElementOf(path, kConditions);
            const StringView field = LastSegment(path);
            Edit(u8"edit_animgraph_transition", [l, at, path, field, condition, number, real, on](pipeline::AnimationGraphAsset& asset)
                 {
                     animation::GraphTransitionData& transition = asset.source.layers[l].transitions[at];
                     if (condition >= 0)
                     {
                         if (static_cast<usize>(condition) >= transition.conditions.Size())
                             return;
                         animation::GraphConditionData& changed = transition.conditions[static_cast<usize>(condition)];
                         if (field == StringView(u8"parameter"))
                             changed.paramIndex = static_cast<i32>(number);
                         else if (field == StringView(u8"compare"))
                             changed.op = static_cast<u8>(Clamp<i64>(number, 0, 5));
                         else if (field == StringView(u8"value"))
                             changed.threshold = static_cast<f32>(real);
                         return;
                     }
                     if (path == StringView(u8"duration"))
                         transition.duration = static_cast<f32>(Max(real, 0.0));
                     else if (path == StringView(u8"exit"))
                         transition.hasExitTime = on;
                     else if (path == StringView(u8"exitTime"))
                         transition.exitTime = static_cast<f32>(Clamp(real, 0.0, 1.0));
                     else if (path == StringView(u8"priority"))
                         transition.priority = static_cast<i32>(number);
                 });
        }
    }

    void AnimationGraphEditorPage::ListEdited(gui::PropertyBag& rows, StringView path, const gui::ListEdit& edit)
    {
        pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr)
            return;
        const usize l = static_cast<usize>(m_layer);
        const usize at = static_cast<usize>(Max(m_chosen.index, 0));
        const bool insert = edit.Kind == gui::ListEdit::Op::Insert;
        const usize index = edit.Index;
        if (&rows == m_stateRows.Get() && path == kEntries && at < graph->source.layers[l].states.Size())
        {
            if (!insert && edit.Kind != gui::ListEdit::Op::Remove)
                return;
            Edit(u8"edit_animgraph_state", [l, at, insert, index](pipeline::AnimationGraphAsset& asset)
                 {
                     animation::GraphNodeData& node = asset.source.layers[l].states[at].node;
                     SquareEntries(node);
                     if (insert)
                     {
                         // A new threshold goes one past the last.
                         node.entryThresholds.PushBack(node.entryThresholds.IsEmpty() ? 0.0f : node.entryThresholds.Back() + 1.0f);
                         node.entryPositions.PushBack(Float2{});
                         node.entryClips.PushBack(Guid{});
                     }
                     else if (index < node.entryClips.Size())
                     {
                         node.entryClips.RemoveAt(index);
                         node.entryThresholds.RemoveAt(index);
                         node.entryPositions.RemoveAt(index);
                     }
                 });
        }
        else if (&rows == m_transitionRows.Get() && path == kConditions && at < graph->source.layers[l].transitions.Size())
        {
            if (!insert && edit.Kind != gui::ListEdit::Op::Remove)
                return;
            Edit(u8"edit_animgraph_transition", [l, at, insert, index](pipeline::AnimationGraphAsset& asset)
                 {
                     auto& conditions = asset.source.layers[l].transitions[at].conditions;
                     if (insert)
                         conditions.PushBack(animation::GraphConditionData{0, 0, 1.0f});
                     else if (index < conditions.Size())
                         conditions.RemoveAt(index);
                 });
        }
        ShowRows(); // show the list as the graph holds it, undoing any move (these lists do not offer moves)
    }

    void AnimationGraphEditorPage::LiveCommitted(StringView path, const gui::Variant& value)
    {
        const Optional<i64> index = path.StartsWith(u8"p") ? ParseInt(path.SubStr(1, path.Size() - 1)) : Optional<i64>{};
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (!index.HasValue() || graph == nullptr || index.Value() < 0 || static_cast<usize>(index.Value()) >= graph->source.paramNames.Size())
            return;
        const usize p = static_cast<usize>(index.Value());
        const u8 type = p < graph->source.paramTypes.Size() ? graph->source.paramTypes[p] : 0;
        if (type == 3)
        {
            if (value.AsBool())
                SetLive(static_cast<i32>(p), 1.0);
            m_liveRows->SetValue(path, 0, gui::Variant(false)); // ready to fire again
            return;
        }
        SetLive(static_cast<i32>(p), type == 2 ? (value.AsBool() ? 1.0 : 0.0) : type == 1 ? static_cast<f64>(value.AsInt()) : value.AsFloat());
    }

    void AnimationGraphEditorPage::SetLive(i32 parameter, f64 value)
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr || parameter < 0 || static_cast<usize>(parameter) >= m_live.Size())
            return;
        const u8 type = static_cast<usize>(parameter) < graph->source.paramTypes.Size() ? graph->source.paramTypes[static_cast<usize>(parameter)] : 0;
        if (type == 3)
        {
            if (m_player)
                m_player->SetTrigger(parameter);
            return;
        }
        m_live[static_cast<usize>(parameter)] = value;
        ApplyLive();
    }

    // === The lists and the view ===

    void AnimationGraphEditorPage::ShowLists()
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr)
            return;
        m_showing = true;
        m_layerModel->Clear();
        for (const animation::GraphLayerData& layer : graph->source.layers)
        {
            const i32 node = m_layerModel->AddNode(gui::kTreeModelRoot, layer.name.IsEmpty() ? StringView(u8"Layer") : layer.name.AsView());
            m_layerModel->SetIcon(node, u8"Editor/Icons/layers.svg");
        }
        m_parameterModel->Clear();
        for (usize p = 0; p < graph->source.paramNames.Size(); ++p)
        {
            const u8 type = p < graph->source.paramTypes.Size() ? graph->source.paramTypes[p] : 0;
            (void)m_parameterModel->AddNode(gui::kTreeModelRoot, Format(u8"{}   {}", graph->source.paramNames[p], kParameterTypes[Min<u8>(type, 3)]).AsView());
        }
        if (m_layerTree != nullptr)
        {
            const gui::ItemId id = static_cast<gui::ItemId>(m_layer);
            m_layerTree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
        }
        if (m_parameterTree != nullptr)
        {
            if (m_chosen.kind == GraphChoiceKind::Parameter)
            {
                const gui::ItemId id = static_cast<gui::ItemId>(m_chosen.index);
                m_parameterTree->SetSelectedIds(Span<const gui::ItemId>(&id, 1u));
            }
            else
                m_parameterTree->SetSelectedIds(Span<const gui::ItemId>{});
        }
        m_showing = false;
    }

    String AnimationGraphEditorPage::LayerList() const
    {
        String out;
        if (const pipeline::AnimationGraphAsset* graph = Graph())
            for (const animation::GraphLayerData& layer : graph->source.layers)
            {
                if (!out.IsEmpty())
                    out += u8"\n";
                out += layer.name.AsView();
            }
        return out;
    }

    String AnimationGraphEditorPage::ParameterList() const
    {
        String out;
        if (const pipeline::AnimationGraphAsset* graph = Graph())
            for (usize p = 0; p < graph->source.paramNames.Size(); ++p)
            {
                if (!out.IsEmpty())
                    out += u8"\n";
                const u8 type = p < graph->source.paramTypes.Size() ? graph->source.paramTypes[p] : 0;
                out += Format(u8"{}  {}", graph->source.paramNames[p], kParameterTypes[Min<u8>(type, 3)]).AsView();
            }
        return out;
    }

    void AnimationGraphEditorPage::FollowView()
    {
        if (m_showing || m_view == nullptr)
            return;
        const Array<gui::GraphId>& nodes = m_view->GetSelectedNodes();
        const Array<gui::GraphId>& links = m_view->GetSelectedLinks();
        if (nodes.Size() == 1 && links.IsEmpty() && m_states->StateOf(nodes[0]) >= 0)
            Choose(GraphChoice{GraphChoiceKind::State, m_states->StateOf(nodes[0])});
        else if (links.Size() == 1 && nodes.IsEmpty() && m_states->TransitionOf(links[0]) >= 0)
            Choose(GraphChoice{GraphChoiceKind::Transition, m_states->TransitionOf(links[0])});
        else if (nodes.IsEmpty() && links.IsEmpty())
            Choose(GraphChoice{GraphChoiceKind::Layer, m_layer});
    }

    void AnimationGraphEditorPage::ChooseLayer(i32 layer)
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr || graph->source.layers.IsEmpty())
            return;
        m_layer = Clamp(layer, 0, static_cast<i32>(graph->source.layers.Size()) - 1);
        m_states->SetLayer(m_layer);
        m_lit = 0;
        m_graphFramed = false; // frame the new layer's states once they are shown
        Choose(GraphChoice{GraphChoiceKind::Layer, m_layer});
    }

    void AnimationGraphEditorPage::Choose(const GraphChoice& choice)
    {
        m_chosen = choice;
        if (m_chosen.kind == GraphChoiceKind::Layer)
            m_chosen.index = m_layer;
        ShowSections();
        ShowRows();
        ShowLists();
        // Select the state in the view too, when the choice came from a list or from code.
        if (m_view != nullptr && m_chosen.kind == GraphChoiceKind::State)
        {
            m_showing = true;
            const gui::GraphId node = m_states->StateId(m_chosen.index);
            m_view->SetSelectedLinks(Span<const gui::GraphId>{});
            m_view->SetSelectedNodes(Span<const gui::GraphId>(&node, 1u));
            m_showing = false;
        }
    }

    void AnimationGraphEditorPage::AddLayer()
    {
        i32 made = -1;
        Edit(u8"add_animgraph_layer", [&made](pipeline::AnimationGraphAsset& asset) { made = animgraph::AddLayer(asset, u8"Layer"); });
        if (made >= 0)
            ChooseLayer(made);
    }

    bool AnimationGraphEditorPage::RemoveLayer()
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr || graph->source.layers.Size() <= 1)
            return false;
        const i32 layer = m_layer;
        Edit(u8"remove_animgraph_layer", [layer](pipeline::AnimationGraphAsset& asset) { (void)animgraph::RemoveLayer(asset, layer); });
        ChooseLayer(Max(layer - 1, 0));
        return true;
    }

    void AnimationGraphEditorPage::AddParameter(animation::AnimationParameterType type)
    {
        i32 made = -1;
        Edit(u8"add_animgraph_parameter", [&made, type](pipeline::AnimationGraphAsset& asset)
             { made = animgraph::AddParameter(asset, type == animation::AnimationParameterType::Trigger ? StringView(u8"Trigger") : StringView(u8"Parameter"), type); });
        if (made >= 0)
            Choose(GraphChoice{GraphChoiceKind::Parameter, made});
    }

    bool AnimationGraphEditorPage::RemoveParameter()
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph == nullptr || m_chosen.kind != GraphChoiceKind::Parameter || m_chosen.index < 0 ||
            static_cast<usize>(m_chosen.index) >= graph->source.paramNames.Size())
            return false;
        const i32 parameter = m_chosen.index;
        Edit(u8"remove_animgraph_parameter", [parameter](pipeline::AnimationGraphAsset& asset) { (void)animgraph::RemoveParameter(asset, parameter); });
        Choose(GraphChoice{GraphChoiceKind::Layer, m_layer});
        return true;
    }

    void AnimationGraphEditorPage::OnObjectChanged()
    {
        // After an edit, undo or redo, read the view, lists and rows again and rebuild the preview.
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (graph != nullptr && !graph->source.layers.IsEmpty() && m_layer >= static_cast<i32>(graph->source.layers.Size()))
        {
            m_layer = static_cast<i32>(graph->source.layers.Size()) - 1;
            m_states->SetLayer(m_layer);
        }
        m_states->Refresh();
        // What was chosen may be gone.
        if (graph != nullptr && !graph->source.layers.IsEmpty())
        {
            const animation::GraphLayerData& layer = graph->source.layers[static_cast<usize>(m_layer)];
            const usize count = m_chosen.kind == GraphChoiceKind::Parameter ? graph->source.paramNames.Size()
                              : m_chosen.kind == GraphChoiceKind::State     ? layer.states.Size()
                              : m_chosen.kind == GraphChoiceKind::Transition ? layer.transitions.Size()
                                                                             : graph->source.layers.Size();
            if (m_chosen.index < 0 || static_cast<usize>(m_chosen.index) >= count)
                m_chosen = GraphChoice{GraphChoiceKind::Layer, m_layer};
        }
        BuildLiveRows();
        ShowLists();
        ShowRows();
        ShowFacts();
        m_rebuild = true;
    }

    // === The preview ===

    void AnimationGraphEditorPage::LoadPreviewPref()
    {
        if (!m_rig)
            return;
        if (foundation::settings::Settings* store = Context().ProjectEditorSettings())
            if (const GraphPreviewSettings* section = store->Find<GraphPreviewSettings>())
                for (const GraphPreviewPref& p : section->prefs)
                    if (p.asset == InstanceId())
                    {
                        m_rig->SetSkeleton(p.skeleton);
                        m_rig->SetMesh(p.mesh);
                        return;
                    }
        // Never previewed before: use the skeleton its first clip's import names.
        Guid skeleton;
        const pipeline::AnimationGraphAsset* graph = Graph();
        foundation::content::ContentDatabase* db = Context().Project() != nullptr ? &Context().Project()->SourceDb() : nullptr;
        const auto skeletonOf = [db](const Guid& clip) -> Guid
        {
            foundation::content::Instance* instance = db != nullptr && !clip.IsNil() ? db->GetInstance(clip) : nullptr;
            RefPtr<ISerializable> object = instance != nullptr ? instance->ReadObject() : RefPtr<ISerializable>{};
            const auto* asset = Cast<pipeline::AnimationClipAsset>(object.Get());
            return asset != nullptr ? asset->skeleton : Guid{};
        };
        if (graph != nullptr)
            for (const animation::GraphLayerData& layer : graph->source.layers)
                for (const animation::GraphStateData& state : layer.states)
                {
                    if (skeleton.IsNil())
                        skeleton = skeletonOf(state.node.clipRef);
                    for (const Guid& entry : state.node.entryClips)
                        if (skeleton.IsNil())
                            skeleton = skeletonOf(entry);
                }
        m_rig->SetSkeleton(skeleton);
        m_rig->SetMesh(Guid{});
    }

    void AnimationGraphEditorPage::SavePreviewPref()
    {
        foundation::settings::Settings* store = Context().ProjectEditorSettings();
        if (store == nullptr || !m_rig)
            return;
        GraphPreviewSettings& section = store->Section<GraphPreviewSettings>();
        bool found = false;
        for (GraphPreviewPref& p : section.prefs)
            if (p.asset == InstanceId())
            {
                p.skeleton = m_rig->SkeletonId();
                p.mesh = m_rig->MeshId();
                found = true;
            }
        if (!found)
            section.prefs.PushBack(GraphPreviewPref{InstanceId(), m_rig->SkeletonId(), m_rig->MeshId()});
        store->MarkChanged<GraphPreviewSettings>();
        Context().RequestProjectEditorSettingsSave();
    }

    void AnimationGraphEditorPage::RebuildPlayer()
    {
        // Torn down in this order because the player borrows the graph and the skeleton.
        m_player.Reset();
        m_graph = RefPtr<animation::AnimationGraph>{};
        m_playerSkeleton = m_rig ? m_rig->Skeleton() : nullptr;
        m_lit = 0;
        const pipeline::AnimationGraphAsset* asset = Graph();
        if (asset == nullptr || Context().Resources() == nullptr || m_playerSkeleton == nullptr || m_playerSkeleton->BoneCount() <= 0)
            return;
        m_graph = MakeRef<animation::AnimationGraph>(Allocator());
        asset->source.BuildInto(*Context().Resources(), *m_graph); // the clips bound by their guids, as the game binds them
        if (m_graph->Layers().IsEmpty())
            return;
        m_player = MakeUnique<animation::AnimationGraphPlayer>(Allocator(), *m_graph, *m_playerSkeleton);
        ApplyLive();
    }

    void AnimationGraphEditorPage::ApplyLive()
    {
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (!m_player || graph == nullptr)
            return;
        for (usize p = 0; p < m_live.Size() && p < graph->source.paramNames.Size(); ++p)
        {
            const u8 type = p < graph->source.paramTypes.Size() ? graph->source.paramTypes[p] : 0;
            const i32 index = static_cast<i32>(p);
            if (type == 0)
                m_player->SetFloat(index, static_cast<f32>(m_live[p]));
            else if (type == 1)
                m_player->SetInt(index, static_cast<i32>(m_live[p]));
            else if (type == 2)
                m_player->SetBool(index, m_live[p] != 0.0);
        }
    }

    void AnimationGraphEditorPage::Pose(f32 dt)
    {
        if (!m_rig)
            return;
        if (m_rebuild || (m_rig->Skeleton() != m_playerSkeleton))
        {
            m_rebuild = false;
            RebuildPlayer();
        }
        if (m_pane)
        {
            if (m_rig->Skeleton() == nullptr)
                m_pane->ShowEmpty(u8"No skeleton to play it on", u8"Pick the skeleton its clips belong to on the preview's bar.");
            else
                m_pane->Show();
        }
        if (!m_player)
        {
            m_rig->Show(Span<const animation::BoneTransform>{});
            return;
        }
        if (m_playing)
            m_player->Update(dt);
        // Consume the root motion every frame so it never accumulates. Apply it while travelling,
        // and return the rig to the middle once it has gone a few metres.
        const animation::RootMotionDelta moved = m_player->ConsumeRootMotion();
        if (m_travel)
        {
            m_travelPosition = m_travelPosition + RotateVector(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, m_travelYaw), moved.translation);
            m_travelYaw += moved.yaw;
            if (Length(Float3{m_travelPosition.x, 0.0f, m_travelPosition.z}) > 6.0f)
                m_travelPosition = Float3{};
        }
        const Float4x4 base = RotationMatrix(Quaternion::FromAxisAngle(Float3{0.0f, 1.0f, 0.0f}, m_travelYaw)) * Float4x4::Translation(m_travelPosition);
        m_rig->Show(m_player->GetLocalPoses(), base);

        // Light the playing state's node on the graph.
        const i32 layers = static_cast<i32>(m_graph->Layers().Size());
        const i32 current = m_player->GetCurrentStateIndex(Min(m_layer, layers - 1));
        const gui::GraphId lit = current >= 0 ? m_states->StateId(current) : 0;
        if (lit != m_lit && m_view != nullptr)
        {
            m_lit = lit;
            m_view->SetHighlightedNodes(lit != 0 ? Span<const gui::GraphId>(&m_lit, 1u) : Span<const gui::GraphId>{});
        }
    }

    void AnimationGraphEditorPage::ShowTransport()
    {
        if (m_state == nullptr)
            return;
        String text;
        const pipeline::AnimationGraphAsset* graph = Graph();
        if (!m_player)
            text = String(m_rig && m_rig->Skeleton() == nullptr ? StringView(u8"No skeleton") : StringView(u8"Not cooked yet"));
        else if (graph != nullptr)
        {
            const i32 layers = static_cast<i32>(m_graph->Layers().Size());
            const i32 layer = Min(m_layer, layers - 1);
            const i32 current = m_player->GetCurrentStateIndex(layer);
            const auto& states = graph->source.layers[static_cast<usize>(m_layer)].states;
            text = current >= 0 && static_cast<usize>(current) < states.Size() ? String(states[static_cast<usize>(current)].name.AsView()) : String(u8"No state");
            if (m_player->IsTransitioning(layer))
                text += u8" (blending in)";
        }
        if (m_state->GetText() != text.AsView())
            m_state->SetText(text.AsView());
        if (m_playing)
        {
            m_state->AddClass(u8"playing");
            m_state->RemoveClass(u8"paused");
        }
        else
        {
            m_state->AddClass(u8"paused");
            m_state->RemoveClass(u8"playing");
        }
    }

    void AnimationGraphEditorPage::FramePreview()
    {
        if (m_rig && m_rig->Frame())
            return;
        if (m_pane && m_pane->Viewport() != nullptr)
        {
            m_pane->Viewport()->Camera().position = Float3{0.0f, 1.4f, 3.2f};
            m_pane->Viewport()->Camera().LookAt(Float3{0.0f, 0.9f, 0.0f});
        }
    }

    void AnimationGraphEditorPage::Stop()
    {
        // Restart the graph: build a new player, keeping the parameter values the preview set.
        m_playing = false;
        m_travelPosition = Float3{};
        m_travelYaw = 0.0f;
        m_rebuild = true;
    }

    void AnimationGraphEditorPage::Restart()
    {
        Stop();
        m_playing = true;
    }

    void AnimationGraphEditorPage::OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt)
    {
        app::AssetObjectPage::OnUpdate(host, dt);
        // Frame the layer's states once the view has a size (the first frame it is laid out).
        if (!m_graphFramed && m_view != nullptr && m_view->GetContentBounds().width > 0.0f)
        {
            m_graphFramed = true;
            m_view->FrameAll();
        }
        Pose(dt);
        ShowTransport();
        if (m_pane)
            m_pane->Update(dt);
    }

    void AnimationGraphEditorPage::OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame)
    {
        if (m_pane)
            m_pane->Render(frame);
    }

    void AnimationGraphEditorPage::OnClose()
    {
        if (m_pane)
            m_pane->Close();
    }

    // === The factory ===

    const TypeInfo* AnimationGraphPageFactory::PrimaryType() const
    {
        return &pipeline::AnimationGraphAsset::StaticType();
    }

    UniquePtr<EditorPage> AnimationGraphPageFactory::CreatePage(EditorContext& context, foundation::content::Instance& instance)
    {
        auto* page = editor::EditorRootAllocator().New<AnimationGraphEditorPage>(context, m_services->Make(), instance);
        return UniquePtr<EditorPage>(page, editor::EditorRootAllocator());
    }

    void RegisterAnimationGraphEditor(EditorContext& context, const app::AssetPageServicesSource& services)
    {
        // The settings section for preview choices, registered before the app reads the per-project
        // store.
        GlobalTypeRegistry().Register(GraphPreviewSettings::StaticType(), TypeDomain(u8"Editor"));
        RegisterSerializable<GraphPreviewSettings>();
        context.Pages().Register(UniquePtr<IEditorPageFactory>(editor::EditorRootAllocator().New<AnimationGraphPageFactory>(services),
                                                               editor::EditorRootAllocator()));
    }

    RTTI_DEFINE_OBJECT_VERSIONED(GraphPreviewSettings, "rtti::editor::editor.graph", 1)

    void AnimationGraphEditorPage::ShowListMenu(bool layers, i32 index, gui::Node& owner, Float2 at)
    {
        if (layers)
            ChooseLayer(index);
        else
            Choose(GraphChoice{GraphChoiceKind::Parameter, index});
        AnimationGraphEditorPage* self = this;
        m_listMenu = MakeRef<gui::Menu>(Allocator());
        gui::MenuItem* remove = m_listMenu->AddItem(layers ? StringView(u8"Delete Layer") : StringView(u8"Delete Parameter"), [self, layers]()
                                                    {
                                                        if (layers)
                                                            (void)self->RemoveLayer();
                                                        else
                                                            (void)self->RemoveParameter();
                                                    });
        const pipeline::AnimationGraphAsset* graph = Graph();
        remove->SetEnabled(!layers || (graph != nullptr && graph->source.layers.Size() > 1)); // a graph keeps at least one layer
        m_listMenu->Open(owner, at);
    }
}
