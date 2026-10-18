// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :animation_graph_page partition.
//
// AnimationGraphEditorPage: the blend-tree / state-machine
// authoring tool for an AnimationGraphAsset.
//
//   +----------------+--------------------------------------+---------------------+
//   | Layers         |  NodeGraphCanvas (selected layer)    |  inspector for the  |
//   |  + add/select  |   states = nodes, transitions =      |  selected state /   |
//   | Parameters     |   straight arrowed edges, Any State  |  transition / layer |
//   |  + add/select  |   pseudo-node; right-click authors   |  / parameter        |
//   +----------------+--------------------------------------+---------------------+
//
// The canvas runs in ConnectionStyle::StraightNodeToNode (the state-machine style added for this
// page): node 0 is the per-layer "Any State" pseudo-node, node 1+i is state i, and connection
// index == transition index (built in transition order), so canvas events map back losslessly.
// Node positions persist on the ASSET (layerStatePositions / layerAnyStatePositions - editor-only
// data never rides the cooked source). Edits mutate the authored source in place and record
// coalesced whole-asset blob-snapshot undo; structural edits defer canvas + inspector rebuilds
// through the UI mutation queue and reselect by identity. Save writes the asset + recooks.

module;
#include "Core/Prelude.h"

export module editor.scene:animation_graph_page;

import foundation.core;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.animation;
import foundation.animation.resource;
import animation.pipeline;
import foundation.geometry; // StaticMesh (the optional skinned preview mesh)
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.toolkit;
import foundation.ui.runtime;
import foundation.ui.viewport;
import foundation.vg.renderer;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace vg = foundation::vg;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace animation = foundation::animation;

    // What the inspector is currently showing. Identity survives rebuilds (indices into the
    // source, re-validated on every use).
    enum class GraphSelKind : u8
    {
        None,
        Layer,
        Parameter,
        State,
        Transition
    };

    struct GraphSel
    {
        GraphSelKind kind = GraphSelKind::None;
        i32 layer = 0;  // owning layer (Layer/State/Transition)
        i32 index = -1; // parameter / state / transition index

        [[nodiscard]] bool operator==(const GraphSel& o) const noexcept
        {
            return kind == o.kind && layer == o.layer && index == o.index;
        }
    };

    class AnimationGraphEditorPage final : public app::UIEditorPage, public IPlaybackPage
    {
    public:
        // ---- IPlaybackPage: the preview player; a rebuild is the rewind ----
        [[nodiscard]] bool CanPlay() const override { return m_player.Get() != nullptr; }
        [[nodiscard]] bool IsPlaying() const override { return m_previewPlaying; }
        void Play() override { m_previewPlaying = true; }
        void Pause() override { m_previewPlaying = false; }
        void Stop() override;
        void Restart() override;

        AnimationGraphEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                 ui::runtime::UIHost& uiHost,
                                 foundation::content::Instance& instance);

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override;

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // Record a coalesced undo step for an in-place edit that already happened (merge by key).
        void CommitEdit(StringView mergeKey);

    private:
        // Whole-asset snapshot command (source + canvas layout): same coalescing model as the
        // particle page - Execute() at push time is a no-op (if-changed guard), only a real
        // undo/redo re-deserializes + rebuilds.
        class EditGraphCommand final : public IEditorCommand
        {
        public:
            EditGraphCommand(AnimationGraphEditorPage& page, StringView mergeKey,
                             Array<byte> before, Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }
            [[nodiscard]] bool Execute() override
            {
                m_page->ApplyAssetBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplyAssetBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_animgraph"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditGraphCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            AnimationGraphEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        // --- model access ---
        [[nodiscard]] animation::GraphLayerData* SelectedLayer();
        // Keep the asset's layout arrays exactly parallel to the source (call after any
        // structural change BEFORE using them).
        void SyncLayoutArrays();

        // --- canvas ---
        void RebuildCanvas(); // rebuild nodes+connections for the selected layer
        void RebuildLeftPanel();
        void ShowCanvasMenu(f32 canvasX, f32 canvasY);
        void ShowNodeMenu(i32 nodeIndex);
        void ShowConnectionMenu(i32 connectionIndex);
        // Deferred structural apply: mutate -> undo -> rebuild panels/canvas/inspector ->
        // reselect. Runs through the UI mutation queue (never mid-event).
        void QueueStructural(StringView undoKey, Function<void()> mutate, GraphSel reselect);
        void Select(const GraphSel& sel); // set selection + deferred inspector rebuild

        // --- inspector ---
        void RebuildInspector();
        void BuildLayerInspector(i32 layerIndex);
        void BuildParameterInspector(i32 paramIndex);
        void BuildStateInspector(i32 layerIndex, i32 stateIndex);
        void BuildTransitionInspector(i32 layerIndex, i32 transitionIndex);

        // --- structural internals (index remapping lives here, not in lambdas) ---
        // Remove a state: drops transitions touching it, decrements higher state indices in
        // transitions + defaultState, and keeps the layout arrays parallel.
        void DeleteStateInternal(i32 layerIndex, i32 stateIndex);
        // Remove a parameter: erases the parallel arrays and remaps references (conditions +
        // blend-tree drivers; higher indices decrement, references to IT clear to -1 / drop).
        void DeleteParameterInternal(i32 paramIndex);

        // --- live preview ---
        // The preview scene exists only as a debug-draw + camera surface (no entities): the
        // graph plays through an AnimationGraphPlayer and the skeleton draws as a wireframe.
        // The preview rig's skeleton and skinned mesh (nil clears); the slots show them.
        void SetPreviewSkeleton(const Guid& id);
        // Delete a layer (the last one stays) or a parameter: the row menus and section icons.
        void DeleteLayer(i32 layerIndex);
        void DeleteParam(i32 paramIndex);
        // A transition, or null when the indices name none.
        [[nodiscard]] animation::GraphTransitionData* TransitionAt(i32 layer, i32 transition) const;
        // A state's node, or null when the indices name none.
        [[nodiscard]] animation::GraphNodeData* StateNode(i32 layer, i32 state) const;
        // Persist / restore the preview rig (skeleton + skinned mesh) per graph (project settings).
        void LoadPreviewPref();
        void SavePreviewPref();
        // (Re)build the runtime graph + player from the CURRENT source (clips resolved through
        // the editor's cooked-DB resources). Called after every edit - graphs are tiny.
        void RebuildPreviewGraph();
        void UpdatePreview(f32 dt); // tick + bone wireframe + active-state canvas highlight

        // --- undo ---
        [[nodiscard]] Array<byte> SnapshotAsset() const;
        void ApplyAssetBlob(const Array<byte>& blob);

        [[nodiscard]] ui::UIContext* Ctx() const;

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        RefPtr<pipeline::AnimationGraphAsset> m_asset;

        // views
        RefPtr<ui::toolkit::NodeGraphCanvas> m_canvas;
        RefPtr<ui::FlexLayout> m_leftRows; // layers + parameters rows
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<ui::Label> m_inspectorTitle;
        RefPtr<foundation::ui::View> m_content;

        i32 m_selectedLayer = 0; // the layer shown on the canvas
        GraphSel m_selected;
        Array<byte> m_undoBaseline;
        bool m_syncingCanvas = false; // guard: canvas events ignored during RebuildCanvas

        // preview world (shared substrate hosts a scene; wireframe + optional skinned mesh)
        UniquePtr<PreviewViewport> m_preview;
        void SetPreviewMesh(const Guid& id); // the SkinnedMeshAsset skinned with the graph pose
        RefPtr<app::CompactAssetSlot> m_skeletonSlot; // the preview skeleton
        RefPtr<app::CompactAssetSlot> m_meshSlot;     // the preview mesh
        RefPtr<app::PageToolbar> m_toolbar; // the standard set, playback, bones and mesh
        RefPtr<ui::Label> m_previewStatus; // current state + transition readout

        Guid m_skeletonGuid{};
        foundation::resource::Proxy<animation::Skeleton> m_skeleton;
        RefPtr<animation::AnimationGraph> m_previewGraph; // player borrows it - keep alive
        UniquePtr<animation::AnimationGraphPlayer> m_player;
        animation::Skeleton* m_playerSkeleton = nullptr; // hot-reload guard (pointer identity)
        Array<Float4x4> m_worldScratch;

        // Optional skinned preview mesh: deformed by the graph player's skinning matrices, fed to a
        // MeshComponent each frame. Null = wireframe only.
        Guid m_previewMeshId{};
        foundation::resource::Proxy<foundation::geometry::StaticMesh> m_previewMesh;
        scene::EntityHandle m_meshEntity;
        bool m_showSkeleton = true; // draw the bone wireframe
        bool m_showMesh = true;     // draw the skinned mesh (when one is picked)
        // Travel: the preview walks by the graph's root motion (back to the middle past a few
        // metres), off: it plays in place.
        bool m_showTravel = false;
        Float3 m_travelPosition{};
        f32 m_travelYaw = 0.0f;

        bool m_previewPlaying = true;
        i32 m_lastHighlightedNode = -1; // canvas node with the active-state ring
    };

    class AnimationGraphPageFactory final : public IEditorPageFactory
    {
    public:
        AnimationGraphPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
            : m_host(&host), m_uiHost(&uiHost)
        {
        }

        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Draw a skeleton as a bone wireframe (parent->joint lines + joint crosses) into a debug
    // lane. `worldScratch` is the caller's reusable world-pose buffer. Shared by the animation
    // graph + clip preview pages.
    // `base` places the whole rig (a preview travelling by its root motion).
    void DrawSkeletonWireframe(foundation::render::debug::DebugDraw& draw,
                               animation::Skeleton& skeleton,
                               Span<const animation::BoneTransform> localPoses,
                               Array<Float4x4>& worldScratch,
                               const Float4x4& base = Float4x4::Identity());


    // Registers the AnimationGraph page factory + an "Animation Graph" New-Asset creator.
    void RegisterAnimationGraphEditor(EditorContext& context, runtime::IApplicationHost& host,
                                      ui::runtime::UIHost& uiHost);
}
