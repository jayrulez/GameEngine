// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Animation - the `:graph_page` partition.
//
// AnimationGraphEditorPage: an animation graph on the GUI from its markup
// (Data/Editor/AnimationGraphPage.gml; Documentation/Specs/editor-gui.md, 5.4, layer 10, part 5).
// The graph's layers and parameters are lists, each with its bar (a parameter added by its type);
// the chosen layer is on the graph view (:graph_states: its states the nodes, Any State one more,
// its transitions arrows; a state added from the view's menu, a transition dragged from a state's
// edge); under it the preview plays the graph on the rig the clip page has (a skeleton and a
// skinned mesh, kept per graph; never shown before, the skeleton its first clip's import names),
// with the kit's transport, the state it plays lit on the graph and named beside the transport.
//
// The grid shows what is chosen: a layer (its name, blend, weight and the state it starts in), a
// parameter (its name, type and default), a state (its name, speed, looping and what it plays: a
// clip, or a blend by one or two parameters of entries, each a clip at a threshold or a place) or a
// transition (where it goes, its blend time, its exit time, its priority and its conditions, each a
// parameter compared with a value). The asset keeps its kinds as numbers and its lists in parallel
// arrays, so the rows are the page's own. Each edit is one undo step of the whole asset. Under them,
// the preview's parameters: what the graph plays on, set here to try it (a trigger fired), the
// page's alone and never the graph's.

module;
#include "Core/Prelude.h"

export module editor.animation:graph_page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :preview;
import :graph_states;

using namespace foundation::core;

export namespace editor
{
    /// What the graph page's grid shows.
    enum class GraphChoiceKind : u8
    {
        Layer,
        Parameter,
        State,
        Transition,
    };
    struct GraphChoice
    {
        GraphChoiceKind kind = GraphChoiceKind::Layer;
        i32 index = 0; // the parameter's, the state's or the transition's (in the layer shown)
        [[nodiscard]] bool operator==(const GraphChoice&) const = default;
    };

    class AnimationGraphEditorPage final : public app::AssetObjectPage, public IPlaybackPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/AnimationGraphPage.gml";

        AnimationGraphEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~AnimationGraphEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // === IPlaybackPage: the preview's, never the graph's ===
        [[nodiscard]] bool CanPlay() const override { return m_player.Get() != nullptr; }
        [[nodiscard]] bool IsPlaying() const override { return m_playing; }
        void Play() override { m_playing = true; }
        void Pause() override { m_playing = false; }
        void Stop() override;
        void Restart() override;

        /// The layer on the graph view.
        void ChooseLayer(i32 layer);
        [[nodiscard]] i32 Layer() const noexcept { return m_layer; }
        /// What the grid shows; the lists and the view follow.
        void Choose(const GraphChoice& choice);
        [[nodiscard]] const GraphChoice& Chosen() const noexcept { return m_chosen; }
        void AddLayer();
        bool RemoveLayer();
        void AddParameter(foundation::animation::AnimationParameterType type);
        bool RemoveParameter();

        /// A preview parameter's value, by the parameter's index (a trigger fires): the page's alone.
        void SetLive(i32 parameter, f64 value);

        // === For tests ===
        [[nodiscard]] pipeline::AnimationGraphAsset* Graph() const;
        [[nodiscard]] AnimationGraphStates& States() noexcept { return *m_states; }
        [[nodiscard]] foundation::gui::PropertyBag& LayerRows() noexcept { return *m_layerRows; }
        [[nodiscard]] foundation::gui::PropertyBag& ParameterRows() noexcept { return *m_parameterRows; }
        [[nodiscard]] foundation::gui::PropertyBag& StateRows() noexcept { return *m_stateRows; }
        [[nodiscard]] foundation::gui::PropertyBag& TransitionRows() noexcept { return *m_transitionRows; }
        [[nodiscard]] foundation::gui::PropertyBag& LiveRows() noexcept { return *m_liveRows; }
        /// The lists' rows ("Base" / "Speed  Float"), one a line.
        [[nodiscard]] String LayerList() const;
        [[nodiscard]] String ParameterList() const;
        [[nodiscard]] AnimationPreviewRig* Rig() const noexcept { return m_rig.Get(); }

    protected:
        void OnObjectChanged() override;

    private:
        void BuildRows();
        void BuildLiveRows();
        void ShowLists();
        void ShowRows();
        void ShowSections();
        void ShowFacts();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void ListEdited(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::ListEdit& edit);
        void LiveCommitted(StringView path, const foundation::gui::Variant& value);
        void Edit(StringView typeId, const Function<void(pipeline::AnimationGraphAsset&)>& mutate);
        void FollowView();
        void RebuildPlayer();
        void ApplyLive();
        void Pose(f32 dt);
        void ShowTransport();
        void FramePreview();
        void LoadPreviewPref();
        void SavePreviewPref();
        [[nodiscard]] String ClipName(const Guid& id) const;

        UniquePtr<AnimationGraphStates> m_states;
        i32 m_layer = 0;
        GraphChoice m_chosen;
        bool m_showing = false; // choosing from code: not the user's

        UniquePtr<foundation::gui::PropertyBag> m_layerRows;
        UniquePtr<foundation::gui::PropertyBag> m_parameterRows;
        UniquePtr<foundation::gui::PropertyBag> m_stateRows;
        UniquePtr<foundation::gui::PropertyBag> m_transitionRows;
        UniquePtr<foundation::gui::PropertyBag> m_liveRows; // made again as the parameters change
        usize m_liveCount = static_cast<usize>(-1);         // the parameters it was made for

        foundation::gui::GraphView* m_view = nullptr;
        foundation::gui::TreeView* m_layerTree = nullptr;
        foundation::gui::TreeView* m_parameterTree = nullptr;
        UniquePtr<foundation::gui::TreeModel> m_layerModel;
        UniquePtr<foundation::gui::TreeModel> m_parameterModel;
        foundation::gui::Label* m_state = nullptr;
        foundation::gui::ButtonBase* m_travelButton = nullptr;

        UniquePtr<app::PreviewPane> m_pane;
        UniquePtr<AnimationPreviewRig> m_rig;
        RefPtr<foundation::animation::AnimationGraph> m_graph;          // the player borrows it
        UniquePtr<foundation::animation::AnimationGraphPlayer> m_player;
        foundation::animation::Skeleton* m_playerSkeleton = nullptr;    // what the player was made for
        bool m_rebuild = true;                                           // the graph changed: a new player
        Array<f64> m_live;                                               // the preview's parameter values
        bool m_playing = true;
        bool m_travel = false;
        Float3 m_travelPosition{};
        f32 m_travelYaw = 0.0f;
        foundation::gui::GraphId m_lit = 0; // the node lit as the state played
        bool m_graphFramed = false;         // the view framed to the layer's states, once it has a size
    };

    class AnimationGraphPageFactory final : public IEditorPageFactory
    {
    public:
        explicit AnimationGraphPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The graph page (asking for its services when it is made) and its preview prefs' section. A
    /// new graph comes from Create Asset (Animation.Pipeline's creator).
    void RegisterAnimationGraphEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
