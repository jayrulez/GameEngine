// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:graph_states` partition (implementation).

module;
#include "Core/Prelude.h"

module editor.animation;

import foundation.core;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;

namespace editor
{
    namespace gui = foundation::gui;
    namespace animation = foundation::animation;

    namespace animgraph
    {
        namespace
        {
            [[nodiscard]] bool Valid(const pipeline::AnimationGraphAsset& asset, i32 layer)
            {
                return layer >= 0 && static_cast<usize>(layer) < asset.source.layers.Size();
            }

            // `base`, else `base 2`, `base 3`... the first `taken` says is free.
            [[nodiscard]] String Unique(StringView base, const Function<bool(StringView)>& taken)
            {
                if (!taken(base))
                    return String(base);
                for (i32 n = 2;; ++n)
                {
                    const String name = Format(u8"{} {}", base, n);
                    if (!taken(name.AsView()))
                        return name;
                }
            }
        }

        void SquareLayout(pipeline::AnimationGraphAsset& asset)
        {
            const Array<animation::GraphLayerData>& layers = asset.source.layers;
            while (asset.layerStatePositions.Size() < layers.Size())
                asset.layerStatePositions.PushBack(Array<Float2>{});
            while (asset.layerStatePositions.Size() > layers.Size())
                asset.layerStatePositions.RemoveAt(asset.layerStatePositions.Size() - 1);
            while (asset.layerAnyStatePositions.Size() < layers.Size())
                asset.layerAnyStatePositions.PushBack(Float2{40.0f, 40.0f});
            while (asset.layerAnyStatePositions.Size() > layers.Size())
                asset.layerAnyStatePositions.RemoveAt(asset.layerAnyStatePositions.Size() - 1);
            for (usize l = 0; l < layers.Size(); ++l)
            {
                Array<Float2>& positions = asset.layerStatePositions[l];
                // A state with no place yet fans out to the right, five to a row.
                while (positions.Size() < layers[l].states.Size())
                    positions.PushBack(Float2{240.0f + 200.0f * static_cast<f32>(positions.Size() % 5), 90.0f + 110.0f * static_cast<f32>(positions.Size() / 5)});
                while (positions.Size() > layers[l].states.Size())
                    positions.RemoveAt(positions.Size() - 1);
            }
        }

        String UniqueStateName(const animation::GraphLayerData& layer, StringView base)
        {
            return Unique(base, [&layer](StringView name)
                          {
                              for (const animation::GraphStateData& state : layer.states)
                                  if (state.name.AsView() == name)
                                      return true;
                              return false;
                          });
        }

        i32 AddState(pipeline::AnimationGraphAsset& asset, i32 layer, StringView name, StateKind kind, Float2 at)
        {
            if (!Valid(asset, layer))
                return -1;
            SquareLayout(asset);
            animation::GraphLayerData& data = asset.source.layers[static_cast<usize>(layer)];
            animation::GraphStateData state;
            state.name = UniqueStateName(data, name);
            state.node.kind = static_cast<u8>(kind);
            data.states.PushBack(Move(state));
            asset.layerStatePositions[static_cast<usize>(layer)].PushBack(at);
            return static_cast<i32>(data.states.Size()) - 1;
        }

        bool RemoveState(pipeline::AnimationGraphAsset& asset, i32 layer, i32 state)
        {
            if (!Valid(asset, layer))
                return false;
            SquareLayout(asset);
            animation::GraphLayerData& data = asset.source.layers[static_cast<usize>(layer)];
            if (state < 0 || static_cast<usize>(state) >= data.states.Size() || data.states.Size() <= 1)
                return false;
            data.states.RemoveAt(static_cast<usize>(state));
            // Its transitions go; those after it move down.
            for (usize t = data.transitions.Size(); t-- > 0;)
            {
                animation::GraphTransitionData& transition = data.transitions[t];
                if (transition.src == state || transition.dst == state)
                {
                    data.transitions.RemoveAt(t);
                    continue;
                }
                if (transition.src > state)
                    --transition.src;
                if (transition.dst > state)
                    --transition.dst;
            }
            if (data.defaultState == state)
                data.defaultState = 0;
            else if (data.defaultState > state)
                --data.defaultState;
            asset.layerStatePositions[static_cast<usize>(layer)].RemoveAt(static_cast<usize>(state));
            return true;
        }

        i32 AddParameter(pipeline::AnimationGraphAsset& asset, StringView name, animation::AnimationParameterType type)
        {
            animation::AnimationGraphSource& source = asset.source;
            const String unique = Unique(name, [&source](StringView candidate)
                                         {
                                             for (const String& taken : source.paramNames)
                                                 if (taken.AsView() == candidate)
                                                     return true;
                                             return false;
                                         });
            // The parallel arrays as long as the names, whatever an older graph kept.
            const usize count = source.paramNames.Size();
            source.paramTypes.Resize(count, 0);
            source.paramFloats.Resize(count, 0.0f);
            source.paramInts.Resize(count, 0);
            source.paramBools.Resize(count, 0);
            source.paramNames.PushBack(unique);
            source.paramTypes.PushBack(static_cast<u8>(type));
            source.paramFloats.PushBack(0.0f);
            source.paramInts.PushBack(0);
            source.paramBools.PushBack(0);
            return static_cast<i32>(count);
        }

        bool RemoveParameter(pipeline::AnimationGraphAsset& asset, i32 parameter)
        {
            animation::AnimationGraphSource& source = asset.source;
            if (parameter < 0 || static_cast<usize>(parameter) >= source.paramNames.Size())
                return false;
            const usize at = static_cast<usize>(parameter);
            source.paramNames.RemoveAt(at);
            if (at < source.paramTypes.Size())
                source.paramTypes.RemoveAt(at);
            if (at < source.paramFloats.Size())
                source.paramFloats.RemoveAt(at);
            if (at < source.paramInts.Size())
                source.paramInts.RemoveAt(at);
            if (at < source.paramBools.Size())
                source.paramBools.RemoveAt(at);
            // A blend driver reading it reads nothing now; those after it move down.
            const auto remap = [parameter](i32& index)
            {
                if (index == parameter)
                    index = -1;
                else if (index > parameter)
                    --index;
            };
            for (animation::GraphLayerData& layer : source.layers)
            {
                for (animation::GraphStateData& state : layer.states)
                {
                    remap(state.node.paramIndex);
                    remap(state.node.paramIndexX);
                    remap(state.node.paramIndexY);
                }
                // A condition on it says nothing now: it goes.
                for (animation::GraphTransitionData& transition : layer.transitions)
                    for (usize c = transition.conditions.Size(); c-- > 0;)
                    {
                        animation::GraphConditionData& condition = transition.conditions[c];
                        if (condition.paramIndex == parameter)
                            transition.conditions.RemoveAt(c);
                        else if (condition.paramIndex > parameter)
                            --condition.paramIndex;
                    }
            }
            return true;
        }

        i32 AddLayer(pipeline::AnimationGraphAsset& asset, StringView name)
        {
            animation::AnimationGraphSource& source = asset.source;
            SquareLayout(asset);
            animation::GraphLayerData layer;
            layer.name = Unique(name, [&source](StringView candidate)
                                {
                                    for (const animation::GraphLayerData& taken : source.layers)
                                        if (taken.name.AsView() == candidate)
                                            return true;
                                    return false;
                                });
            animation::GraphStateData state;
            state.name = String(u8"State");
            layer.states.PushBack(Move(state));
            source.layers.PushBack(Move(layer));
            SquareLayout(asset);
            return static_cast<i32>(source.layers.Size()) - 1;
        }

        bool RemoveLayer(pipeline::AnimationGraphAsset& asset, i32 layer)
        {
            if (!Valid(asset, layer) || asset.source.layers.Size() <= 1)
                return false;
            SquareLayout(asset);
            asset.source.layers.RemoveAt(static_cast<usize>(layer));
            asset.layerStatePositions.RemoveAt(static_cast<usize>(layer));
            asset.layerAnyStatePositions.RemoveAt(static_cast<usize>(layer));
            return true;
        }
    }

    // === The source ===

    AnimationGraphStates::AnimationGraphStates(Function<pipeline::AnimationGraphAsset*()> asset, EditFunction edit)
        : m_asset(Move(asset)), m_edit(Move(edit))
    {
        Refresh();
    }

    animation::GraphLayerData* AnimationGraphStates::LayerData() const
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (asset == nullptr || m_layer < 0 || static_cast<usize>(m_layer) >= asset->source.layers.Size())
            return nullptr;
        return &asset->source.layers[static_cast<usize>(m_layer)];
    }

    void AnimationGraphStates::SetLayer(i32 layer)
    {
        if (layer == m_layer)
            return;
        m_layer = layer;
        m_stateIds.Clear(); // another layer's states: ids of their own
        m_transitionIds.Clear();
        Refresh();
    }

    void AnimationGraphStates::Refresh()
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (asset != nullptr)
        {
            m_layer = Clamp(m_layer, 0, Max(static_cast<i32>(asset->source.layers.Size()) - 1, 0));
            animgraph::SquareLayout(*asset); // a place for each state before one is drawn
        }
        const animation::GraphLayerData* layer = LayerData();
        const usize states = layer != nullptr ? layer->states.Size() : 0;
        const usize transitions = layer != nullptr ? layer->transitions.Size() : 0;
        // Kept by place: this source's own edits keep the tables in step before they change the
        // asset; another's (an undo) may leave a count off, made good at the end.
        while (m_stateIds.Size() > states)
            m_stateIds.RemoveAt(m_stateIds.Size() - 1);
        while (m_stateIds.Size() < states)
            m_stateIds.PushBack(Next());
        while (m_transitionIds.Size() > transitions)
            m_transitionIds.RemoveAt(m_transitionIds.Size() - 1);
        while (m_transitionIds.Size() < transitions)
            m_transitionIds.PushBack(Next());
        Build();
        NotifyGraphChanged();
    }

    void AnimationGraphStates::Build()
    {
        m_nodes.Clear();
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        const animation::GraphLayerData* layer = LayerData();
        if (asset == nullptr || layer == nullptr)
            return;
        const usize l = static_cast<usize>(m_layer);
        gui::GraphNodeInfo any;
        any.Id = kAnyState;
        any.Type = String(u8"any");
        any.Title = String(u8"Any State");
        any.Subtitle = String(u8"Its transitions start from every state");
        any.Position = l < asset->layerAnyStatePositions.Size() ? asset->layerAnyStatePositions[l] : Float2{40.0f, 40.0f};
        any.HeaderColor = Color{0.42f, 0.43f, 0.5f, 1.0f};
        any.Deletable = false;
        m_nodes.PushBack(Move(any));
        const animation::AnimationGraphSource& source = asset->source;
        const auto parameter = [&source](i32 index)
        {
            return index >= 0 && static_cast<usize>(index) < source.paramNames.Size() ? String(source.paramNames[static_cast<usize>(index)].AsView())
                                                                                      : String(u8"nothing");
        };
        for (usize s = 0; s < layer->states.Size(); ++s)
        {
            const animation::GraphStateData& state = layer->states[s];
            gui::GraphNodeInfo node;
            node.Id = m_stateIds[s];
            node.Title = state.name.IsEmpty() ? Format(u8"State {}", s + 1) : String(state.name.AsView());
            switch (static_cast<animgraph::StateKind>(state.node.kind))
            {
            case animgraph::StateKind::Blend1D:
                node.Type = String(u8"blend1d");
                node.Subtitle = Format(u8"Blends {} by {}", state.node.entryClips.Size(), parameter(state.node.paramIndex));
                break;
            case animgraph::StateKind::Blend2D:
                node.Type = String(u8"blend2d");
                node.Subtitle = Format(u8"Blends {} by {} and {}", state.node.entryClips.Size(), parameter(state.node.paramIndexX), parameter(state.node.paramIndexY));
                break;
            default:
                node.Type = String(u8"clip");
                node.Subtitle = state.node.clipRef.IsNil() ? String(u8"No clip") : (NameOf ? NameOf(state.node.clipRef) : String(u8"A clip"));
                break;
            }
            // The state the layer starts in, apart from the rest.
            if (static_cast<i32>(s) == layer->defaultState)
            {
                node.Subtitle = Format(u8"Starts here · {}", node.Subtitle);
                node.HeaderColor = Color{0.24f, 0.55f, 0.36f, 1.0f};
            }
            node.Position = l < asset->layerStatePositions.Size() && s < asset->layerStatePositions[l].Size() ? asset->layerStatePositions[l][s] : Float2{};
            m_nodes.PushBack(Move(node));
        }
    }

    gui::GraphId AnimationGraphStates::StateId(i32 state) const
    {
        return state >= 0 && static_cast<usize>(state) < m_stateIds.Size() ? m_stateIds[static_cast<usize>(state)] : 0;
    }

    i32 AnimationGraphStates::StateOf(gui::GraphId node) const
    {
        for (usize s = 0; s < m_stateIds.Size(); ++s)
            if (m_stateIds[s] == node)
                return static_cast<i32>(s);
        return -1;
    }

    gui::GraphId AnimationGraphStates::TransitionId(i32 transition) const
    {
        return transition >= 0 && static_cast<usize>(transition) < m_transitionIds.Size() ? m_transitionIds[static_cast<usize>(transition)] : 0;
    }

    i32 AnimationGraphStates::TransitionOf(gui::GraphId link) const
    {
        for (usize t = 0; t < m_transitionIds.Size(); ++t)
            if (m_transitionIds[t] == link)
                return static_cast<i32>(t);
        return -1;
    }

    void AnimationGraphStates::Change(StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate)
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (asset == nullptr)
            return;
        if (m_edit)
            m_edit(typeId, mutate); // the page's step; it refreshes this after
        else
            mutate(*asset);
        Refresh();
    }

    // === What there is ===

    void AnimationGraphStates::GetNodes(Array<gui::GraphId>& out) const
    {
        out.Clear();
        for (const gui::GraphNodeInfo& node : m_nodes)
            out.PushBack(node.Id);
    }

    const gui::GraphNodeInfo* AnimationGraphStates::GetNode(gui::GraphId node) const
    {
        for (const gui::GraphNodeInfo& at : m_nodes)
            if (at.Id == node)
                return &at;
        return nullptr;
    }

    void AnimationGraphStates::GetLinks(Array<gui::GraphLinkInfo>& out) const
    {
        out.Clear();
        const animation::GraphLayerData* layer = LayerData();
        if (layer == nullptr)
            return;
        for (usize t = 0; t < layer->transitions.Size() && t < m_transitionIds.Size(); ++t)
        {
            const animation::GraphTransitionData& transition = layer->transitions[t];
            gui::GraphLinkInfo link;
            link.Id = m_transitionIds[t];
            link.Kind = gui::GraphLinkKind::Nodes;
            link.FromNode = transition.src < 0 ? kAnyState : StateId(transition.src);
            link.ToNode = StateId(transition.dst);
            if (link.FromNode != 0 && link.ToNode != 0)
                out.PushBack(link);
        }
    }

    // === Moving ===

    void AnimationGraphStates::BeginMove()
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (asset == nullptr || LayerData() == nullptr)
            return;
        m_moving = true;
        const usize l = static_cast<usize>(m_layer);
        m_before.Clear();
        m_before.PushBack(asset->layerAnyStatePositions[l]);
        for (const Float2& at : asset->layerStatePositions[l])
            m_before.PushBack(at);
    }

    void AnimationGraphStates::PreviewMove(Span<const gui::GraphId> nodes, Span<const gui::GraphId>, Float2 delta)
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (!m_moving || asset == nullptr || LayerData() == nullptr)
            return;
        const usize l = static_cast<usize>(m_layer);
        // Live in the asset, each from where it was when the move began.
        for (const gui::GraphId id : nodes)
        {
            if (id == kAnyState)
                asset->layerAnyStatePositions[l] = m_before[0] + delta;
            else if (const i32 s = StateOf(id); s >= 0 && static_cast<usize>(s) + 1 < m_before.Size())
                asset->layerStatePositions[l][static_cast<usize>(s)] = m_before[static_cast<usize>(s) + 1] + delta;
        }
        Build();
        NotifyGraphChanged();
    }

    void AnimationGraphStates::CommitMove()
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (!m_moving || asset == nullptr || LayerData() == nullptr)
            return;
        m_moving = false;
        const usize l = static_cast<usize>(m_layer);
        // The step's before is where the move began; its after, where it ended.
        const Float2 anyAfter = asset->layerAnyStatePositions[l];
        const Array<Float2> statesAfter = asset->layerStatePositions[l];
        asset->layerAnyStatePositions[l] = m_before[0];
        for (usize s = 0; s < statesAfter.Size() && s + 1 < m_before.Size(); ++s)
            asset->layerStatePositions[l][s] = m_before[s + 1];
        Change(u8"move_animgraph_states", [l, anyAfter, &statesAfter](pipeline::AnimationGraphAsset& changed)
               {
                   changed.layerAnyStatePositions[l] = anyAfter;
                   changed.layerStatePositions[l] = statesAfter;
               });
    }

    void AnimationGraphStates::CancelMove()
    {
        pipeline::AnimationGraphAsset* asset = m_asset ? m_asset() : nullptr;
        if (!m_moving || asset == nullptr || LayerData() == nullptr)
            return;
        m_moving = false;
        const usize l = static_cast<usize>(m_layer);
        asset->layerAnyStatePositions[l] = m_before[0];
        for (usize s = 0; s < asset->layerStatePositions[l].Size() && s + 1 < m_before.Size(); ++s)
            asset->layerStatePositions[l][s] = m_before[s + 1];
        Build();
        NotifyGraphChanged();
    }

    // === Transitions ===

    gui::GraphLinkCheck AnimationGraphStates::CheckLink(const gui::GraphLinkInfo& link) const
    {
        gui::GraphLinkCheck check;
        if (link.Kind != gui::GraphLinkKind::Nodes)
        {
            check.Reason = String(u8"A transition goes from a state to a state");
            return check;
        }
        const bool fromAny = link.FromNode == kAnyState;
        if (!fromAny && StateOf(link.FromNode) < 0)
        {
            check.Reason = String(u8"Not a state of this layer");
            return check;
        }
        if (link.ToNode == 0)
        {
            check.Allowed = true; // one may begin here
            return check;
        }
        if (link.ToNode == kAnyState)
        {
            check.Reason = String(u8"Any State only begins transitions: nothing goes into it");
            return check;
        }
        if (StateOf(link.ToNode) < 0)
        {
            check.Reason = String(u8"Not a state of this layer");
            return check;
        }
        if (link.ToNode == link.FromNode)
        {
            check.Reason = String(u8"A state cannot go into itself: it is already there");
            return check;
        }
        check.Allowed = true;
        return check;
    }

    gui::GraphId AnimationGraphStates::Connect(const gui::GraphLinkInfo& link)
    {
        if (!CheckLink(link).Allowed || link.ToNode == 0)
            return 0;
        const i32 src = link.FromNode == kAnyState ? -1 : StateOf(link.FromNode);
        const i32 dst = StateOf(link.ToNode);
        const i32 layer = m_layer;
        const gui::GraphId id = Next();
        m_transitionIds.PushBack(id); // in step before the asset changes
        Change(u8"add_animgraph_transition", [layer, src, dst](pipeline::AnimationGraphAsset& asset)
               {
                   animation::GraphTransitionData transition;
                   transition.src = src;
                   transition.dst = dst;
                   asset.source.layers[static_cast<usize>(layer)].transitions.PushBack(Move(transition));
               });
        return id;
    }

    bool AnimationGraphStates::Relink(gui::GraphId link, const gui::GraphLinkInfo& to)
    {
        const i32 transition = TransitionOf(link);
        if (transition < 0 || !CheckLink(to).Allowed || to.ToNode == 0)
            return false;
        const i32 src = to.FromNode == kAnyState ? -1 : StateOf(to.FromNode);
        const i32 dst = StateOf(to.ToNode);
        const i32 layer = m_layer;
        Change(u8"edit_animgraph_transition", [layer, transition, src, dst](pipeline::AnimationGraphAsset& asset)
               {
                   animation::GraphTransitionData& changed = asset.source.layers[static_cast<usize>(layer)].transitions[static_cast<usize>(transition)];
                   changed.src = src;
                   changed.dst = dst;
               });
        return true;
    }

    bool AnimationGraphStates::Disconnect(Span<const gui::GraphId> links)
    {
        Array<i32> gone;
        for (const gui::GraphId link : links)
            if (const i32 t = TransitionOf(link); t >= 0)
                gone.PushBack(t);
        if (gone.IsEmpty())
            return false;
        // Highest first, so each removal leaves the others' places as they were.
        for (usize i = 0; i < gone.Size(); ++i)
            for (usize j = i + 1; j < gone.Size(); ++j)
                if (gone[j] > gone[i])
                {
                    const i32 higher = gone[j];
                    gone[j] = gone[i];
                    gone[i] = higher;
                }
        for (const i32 t : gone)
            m_transitionIds.RemoveAt(static_cast<usize>(t));
        const i32 layer = m_layer;
        Change(u8"remove_animgraph_transition", [layer, &gone](pipeline::AnimationGraphAsset& asset)
               {
                   for (const i32 t : gone)
                       asset.source.layers[static_cast<usize>(layer)].transitions.RemoveAt(static_cast<usize>(t));
               });
        return true;
    }

    // === States ===

    bool AnimationGraphStates::DeleteNodes(Span<const gui::GraphId> nodes)
    {
        const animation::GraphLayerData* layer = LayerData();
        if (layer == nullptr)
            return false;
        Array<i32> gone;
        for (const gui::GraphId node : nodes)
            if (const i32 s = StateOf(node); s >= 0)
                gone.PushBack(s);
        for (usize i = 0; i < gone.Size(); ++i)
            for (usize j = i + 1; j < gone.Size(); ++j)
                if (gone[j] > gone[i])
                {
                    const i32 higher = gone[j];
                    gone[j] = gone[i];
                    gone[i] = higher;
                }
        // A layer keeps one state: the first of them stays when all were asked for.
        while (!gone.IsEmpty() && gone.Size() >= layer->states.Size())
            gone.RemoveAt(gone.Size() - 1);
        if (gone.IsEmpty())
            return false;
        // The ids in step: the states', and the transitions' that go with them.
        Array<bool> transitionGoes;
        transitionGoes.Resize(layer->transitions.Size(), false);
        for (usize t = 0; t < layer->transitions.Size(); ++t)
            for (const i32 s : gone)
                transitionGoes[t] = transitionGoes[t] || layer->transitions[t].src == s || layer->transitions[t].dst == s;
        for (usize t = transitionGoes.Size(); t-- > 0;)
            if (transitionGoes[t] && t < m_transitionIds.Size())
                m_transitionIds.RemoveAt(t);
        for (const i32 s : gone)
            m_stateIds.RemoveAt(static_cast<usize>(s));
        const i32 index = m_layer;
        Change(u8"remove_animgraph_state", [index, &gone](pipeline::AnimationGraphAsset& asset)
               {
                   for (const i32 s : gone)
                       (void)animgraph::RemoveState(asset, index, s);
               });
        return true;
    }

    void AnimationGraphStates::GetNodeTypes(gui::GraphId, Array<gui::GraphNodeType>& out) const
    {
        out.Clear();
        out.PushBack(gui::GraphNodeType{String(u8"clip"), String(u8"Clip State"), String(u8"States")});
        out.PushBack(gui::GraphNodeType{String(u8"blend1d"), String(u8"Blend by One Parameter"), String(u8"States")});
        out.PushBack(gui::GraphNodeType{String(u8"blend2d"), String(u8"Blend by Two Parameters"), String(u8"States")});
    }

    gui::GraphId AnimationGraphStates::CreateNode(StringView type, Float2 at, gui::GraphId)
    {
        const animation::GraphLayerData* layer = LayerData();
        if (layer == nullptr)
            return 0;
        const animgraph::StateKind kind = type == StringView(u8"blend1d")   ? animgraph::StateKind::Blend1D
                                          : type == StringView(u8"blend2d") ? animgraph::StateKind::Blend2D
                                          : type == StringView(u8"clip")    ? animgraph::StateKind::Clip
                                                                            : static_cast<animgraph::StateKind>(255);
        if (static_cast<u8>(kind) == 255)
            return 0;
        const StringView base = kind == animgraph::StateKind::Clip ? StringView(u8"State") : StringView(u8"Blend");
        const i32 index = m_layer;
        const gui::GraphId id = Next();
        m_stateIds.PushBack(id); // in step before the asset changes
        Change(u8"add_animgraph_state", [index, base, kind, at](pipeline::AnimationGraphAsset& asset) { (void)animgraph::AddState(asset, index, base, kind, at); });
        return id;
    }
}
