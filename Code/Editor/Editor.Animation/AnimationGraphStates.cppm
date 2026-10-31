// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:graph_states` partition.
//
// An animation graph as a graph view reads it, with no widget of its own. AnimationGraphStates is
// a graph source over one layer of an AnimationGraphAsset: its states are the nodes, and Any State
// is a node of its own that can only begin a transition. Its transitions are node links (a state
// machine's, which the view draws as arrows between borders). Node positions are kept in the
// asset. Ids stay with what they name while states and transitions come and go (the asset
// addresses both by index, and a removal shifts the rest), so a view's selection survives an edit.
//
// Every change is one edit of the whole asset, made through the page's Edit, which takes the undo
// step. A move is a gesture: it is previewed in the asset and taken as one undo step when it ends.
// Deleting a state also deletes its transitions and shifts down the indices after it; a layer
// always keeps one state. The graph edits the view does not make (adding or removing a layer or a
// parameter, and resizing the layout to the states) are the free functions below, so the page's
// lists can share them. Removing a parameter deletes the conditions on it, clears the blend
// drivers that read it, and shifts down the indices of the parameters after it.

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

        /// Gives every layer one position per state (a new state is placed beside the last) and
        /// one Any State position per layer.
        void SquareLayout(pipeline::AnimationGraphAsset& asset);
        /// A name among the layer's states not taken: `base`, else `base 2`, `base 3`...
        [[nodiscard]] String UniqueStateName(const foundation::animation::GraphLayerData& layer, StringView base);
        /// Adds a state of `kind` named `name` at `at` to `layer`; returns its index.
        i32 AddState(pipeline::AnimationGraphAsset& asset, i32 layer, StringView name, StateKind kind, Float2 at);
        /// Removes `state` from `layer` with its transitions, and shifts down the indices after it
        /// (in the transitions, the default state and the positions). Returns false for the layer's
        /// last state.
        bool RemoveState(pipeline::AnimationGraphAsset& asset, i32 layer, i32 state);
        /// Adds a parameter of `type` named `name` (made unique); returns its index.
        i32 AddParameter(pipeline::AnimationGraphAsset& asset, StringView name, foundation::animation::AnimationParameterType type);
        /// Removes `parameter`: conditions on it are deleted, blend drivers that read it are
        /// cleared, and the indices of the parameters after it shift down.
        bool RemoveParameter(pipeline::AnimationGraphAsset& asset, i32 parameter);
        /// Adds a layer named `name` (made unique) with one clip state; returns its index.
        i32 AddLayer(pipeline::AnimationGraphAsset& asset, StringView name);
        /// Removes `layer` and its positions; returns false for the last layer.
        bool RemoveLayer(pipeline::AnimationGraphAsset& asset, i32 layer);
    }

    /// AnimationGraphStates::kAnyState, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr foundation::gui::GraphId kAnimationGraphStatesAnyState = 1;

    class AnimationGraphStates final : public foundation::gui::IGraphSource
    {
    public:
        /// Any State's node id, in every layer.
        static constexpr foundation::gui::GraphId kAnyState = kAnimationGraphStatesAnyState;
        /// How the source changes the asset: applies `mutate` as one undo step named `typeId`.
        using EditFunction = Function<void(StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate)>;

        /// A source over the asset that `asset` returns. It is asked each time, because an undo
        /// replaces the asset. Changes go through `edit`, or are made in place when there is none.
        AnimationGraphStates(Function<pipeline::AnimationGraphAsset*()> asset, EditFunction edit);

        /// Sets the layer it shows (clamped to the asset's layers).
        void SetLayer(i32 layer);
        [[nodiscard]] i32 Layer() const noexcept { return m_layer; }
        /// Reads the asset again after it changed elsewhere (an undo, or an edit from the page).
        /// Ids are kept by index where the counts still match.
        void Refresh();
        /// The name a state's node shows for a clip's guid ("idle"). When unset, the node says only
        /// that it has a clip.
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
        // reads would carry little, and the page's Add command makes a new one.
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
        Array<Float2> m_before; // positions when a move began: Any State's, then each state's
    };
}
