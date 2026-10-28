// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:graph_states` partition.
//
// An animation graph as a graph view reads it (Documentation/Specs/editor-gui.md, 5.4, layer 10,
// part 4), no widget of its own. AnimationGraphStates is a graph source over one layer of an
// AnimationGraphAsset: its states are the nodes, and Any State a node of its own that only begins
// a transition; its transitions are node links (a state machine's, which the view draws as arrows
// between borders); where each sits is the asset's, as the old page kept it. Ids stay with what
// they name while states and transitions come and go (the asset addresses both by place, and a
// removal shifts the rest), so a view's selection survives an edit.
//
// Every change is one edit of the whole asset, through the page's Edit (its undo step); a move is
// a gesture, previewed in the asset and taken as one step when it ends. A state deleted takes its
// transitions with it and moves the indices after it down; a layer keeps one state. The graph
// edits that are not the view's (a layer or a parameter added or removed, the layout squared to
// the states) are the free functions below, so the page's lists share them: a parameter removed
// takes the conditions on it away and clears the blend drivers that read it, those after it moving
// down.

module;
#include "Core/Prelude.h"

export module editor.animation:graph_states;

import foundation.core;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;

using namespace foundation::core;

export namespace editor
{
    namespace animgraph
    {
        /// The node kinds a state may have, as the asset keeps them (GraphNodeData::kind).
        enum class StateKind : u8
        {
            Clip = 0,
            Blend1D = 1,
            Blend2D = 2,
        };

        /// Every layer's positions as many as its states (a new state placed beside the last),
        /// and one Any State place per layer.
        void SquareLayout(pipeline::AnimationGraphAsset& asset);
        /// A name among the layer's states not taken: `base`, else `base 2`, `base 3`...
        [[nodiscard]] String UniqueStateName(const foundation::animation::GraphLayerData& layer, StringView base);
        /// Adds a state of `kind` named `name` at `at` to `layer`; its index.
        i32 AddState(pipeline::AnimationGraphAsset& asset, i32 layer, StringView name, StateKind kind, Float2 at);
        /// Removes `state` from `layer`: its transitions with it, the indices after it moved down
        /// (the transitions', the default state's, the positions'). False when it is the last.
        bool RemoveState(pipeline::AnimationGraphAsset& asset, i32 layer, i32 state);
        /// Adds a parameter of `type` named `name` (made unique); its index.
        i32 AddParameter(pipeline::AnimationGraphAsset& asset, StringView name, foundation::animation::AnimationParameterType type);
        /// Removes `parameter`: conditions on it go, blend drivers reading it clear, those after
        /// it move down.
        bool RemoveParameter(pipeline::AnimationGraphAsset& asset, i32 parameter);
        /// Adds a layer named `name` (made unique) with one clip state; its index.
        i32 AddLayer(pipeline::AnimationGraphAsset& asset, StringView name);
        /// Removes `layer` and its places; false when it is the last.
        bool RemoveLayer(pipeline::AnimationGraphAsset& asset, i32 layer);
    }

    class AnimationGraphStates final : public foundation::gui::IGraphSource
    {
    public:
        /// Any State's node id, in every layer.
        static constexpr foundation::gui::GraphId kAnyState = 1;
        /// How it changes the asset: `mutate` applied as one undo step named `typeId`.
        using EditFunction = Function<void(StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate)>;

        /// Over the asset `asset` returns (asked each time: an undo makes it anew), changed through
        /// `edit`; without one, changed in place.
        AnimationGraphStates(Function<pipeline::AnimationGraphAsset*()> asset, EditFunction edit);

        /// The layer it shows (kept within the asset's).
        void SetLayer(i32 layer);
        [[nodiscard]] i32 Layer() const noexcept { return m_layer; }
        /// The asset changed behind it (an undo, an edit of the page's): read again, the ids kept
        /// by place where the counts still match.
        void Refresh();
        /// What a clip's guid is called on a state's node ("idle"); unset: the guid unnamed.
        Function<String(const Guid&)> NameOf;

        [[nodiscard]] foundation::gui::GraphId StateId(i32 state) const;
        /// A state's index by its node id (-1: Any State, or none).
        [[nodiscard]] i32 StateOf(foundation::gui::GraphId node) const;
        [[nodiscard]] foundation::gui::GraphId TransitionId(i32 transition) const;
        /// A transition's index by its link id (-1: none).
        [[nodiscard]] i32 TransitionOf(foundation::gui::GraphId link) const;

        // === IGraphSource ===
        void GetNodes(Array<foundation::gui::GraphId>& out) const override;
        [[nodiscard]] const foundation::gui::GraphNodeInfo* GetNode(foundation::gui::GraphId node) const override;
        void GetLinks(Array<foundation::gui::GraphLinkInfo>& out) const override;
        void GetFrames(Array<foundation::gui::GraphFrameInfo>& out) const override { out.Clear(); } // a state machine keeps none
        void BeginMove() override;
        void PreviewMove(Span<const foundation::gui::GraphId> nodes, Span<const foundation::gui::GraphId> frames, Float2 delta) override;
        void CommitMove() override;
        void CancelMove() override;
        [[nodiscard]] foundation::gui::GraphLinkCheck CheckLink(const foundation::gui::GraphLinkInfo& link) const override;
        foundation::gui::GraphId Connect(const foundation::gui::GraphLinkInfo& link) override;
        bool Relink(foundation::gui::GraphId link, const foundation::gui::GraphLinkInfo& to) override;
        bool Disconnect(Span<const foundation::gui::GraphId> links) override;
        bool DeleteNodes(Span<const foundation::gui::GraphId> nodes) override;
        void GetNodeTypes(foundation::gui::GraphId fittingPin, Array<foundation::gui::GraphNodeType>& out) const override;
        foundation::gui::GraphId CreateNode(StringView type, Float2 at, foundation::gui::GraphId linkFrom) override;
        foundation::gui::GraphId CreateFrame(const foundation::gui::GraphFrameInfo&) override { return 0; }
        bool EditFrame(const foundation::gui::GraphFrameInfo&) override { return false; }
        bool DeleteFrames(Span<const foundation::gui::GraphId>) override { return false; }
        // Copying states is not offered: a state copied without the transitions and parameters it
        // reads would say little, and the page's own Add makes one.
        [[nodiscard]] String CopyNodes(Span<const foundation::gui::GraphId>) const override { return String{}; }
        bool PasteNodes(StringView, Float2, Array<foundation::gui::GraphId>&) override { return false; }

    private:
        [[nodiscard]] foundation::animation::GraphLayerData* LayerData() const;
        void Change(StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate);
        void Build();
        [[nodiscard]] foundation::gui::GraphId Next() noexcept { return m_next++; }

        Function<pipeline::AnimationGraphAsset*()> m_asset;
        EditFunction m_edit;
        i32 m_layer = 0;
        Array<foundation::gui::GraphId> m_stateIds;      // by state index, in the layer shown
        Array<foundation::gui::GraphId> m_transitionIds; // by transition index
        Array<foundation::gui::GraphNodeInfo> m_nodes;   // Any State, then the states
        foundation::gui::GraphId m_next = kAnyState + 1;
        bool m_moving = false;
        Array<Float2> m_before; // a move's: Any State's place, then each state's
    };
}
