// Draconic::EditorScene - :animation_graph_page partition.
//
// AnimationGraphEditorPage (editor-pages-gap.md, bespoke pass #4): the blend-tree / state-machine
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
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.scene:animation_graph_page;

import draconic.foundation;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.scene;
import draconic.engine.scene;
import draconic.animation;
import draconic.animation.resource;
import draconic.animation.editor;
import draconic.resource;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.ui.runtime;
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera;

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace vg = draconic::vg;
    namespace scene = draconic::scene;
    namespace render = draconic::render;
    namespace animation = draconic::animation;

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

    class AnimationGraphEditorPage final : public app::UIEditorPage
    {
    public:
        AnimationGraphEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                 ui::runtime::UIHost& uiHost,
                                 draconic::content::Instance& instance);

        [[nodiscard]] draconic::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override;

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            draconic::graphics::FrameContext& frame) override;
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
        void BuildPreviewScene();
        void PickPreviewSkeleton();
        // (Re)build the runtime graph + player from the CURRENT source (clips resolved through
        // the editor's cooked-DB resources). Called after every edit - graphs are tiny.
        void RebuildPreviewGraph();
        void UpdatePreview(f32 dt); // tick + bone wireframe + active-state canvas highlight
        void EnsureViewportBound();

        // --- undo ---
        [[nodiscard]] Array<byte> SnapshotAsset() const;
        void ApplyAssetBlob(const Array<byte>& blob);

        [[nodiscard]] ui::UIContext* Ctx() const;

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        RefPtr<animation::AnimationGraphAsset> m_asset;

        // views
        RefPtr<ui::toolkit::NodeGraphCanvas> m_canvas;
        RefPtr<ui::FlexLayout> m_leftRows; // layers + parameters rows
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<ui::Label> m_inspectorTitle;
        RefPtr<draconic::ui::View> m_content;

        i32 m_selectedLayer = 0; // the layer shown on the canvas
        GraphSel m_selected;
        Array<byte> m_undoBaseline;
        bool m_syncingCanvas = false; // guard: canvas events ignored during RebuildCanvas

        // preview world (debug-draw only)
        scene::SceneSubsystem* m_scenes = nullptr;
        scene::SceneManager m_sceneManager;
        render::RenderSubsystem* m_render = nullptr;
        scene::Scene* m_scene = nullptr;
        EditorCamera m_camera;
        UniquePtr<draconic::shell::InputRouter> m_router;
        RefPtr<ui::viewport::ViewportView> m_viewport;
        draconic::graphics::RenderWindow* m_hostWindow = nullptr;
        RefPtr<ui::Button> m_skeletonButton; // shows the picked skeleton's name
        RefPtr<ui::Button> m_playButton;
        RefPtr<ui::Label> m_previewStatus; // current state + transition readout

        Guid m_skeletonGuid{};
        draconic::resource::Proxy<animation::Skeleton> m_skeleton;
        RefPtr<animation::AnimationGraph> m_previewGraph; // player borrows it - keep alive
        UniquePtr<animation::AnimationGraphPlayer> m_player;
        animation::Skeleton* m_playerSkeleton = nullptr; // hot-reload guard (pointer identity)
        Array<Float4x4> m_worldScratch;
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
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Draw a skeleton as a bone wireframe (parent->joint lines + joint crosses) into a debug
    // lane. `worldScratch` is the caller's reusable world-pose buffer. Shared by the animation
    // graph + clip preview pages.
    void DrawSkeletonWireframe(draconic::render::debug::DebugDraw& draw,
                               animation::Skeleton& skeleton,
                               Span<const animation::BoneTransform> localPoses,
                               Array<Float4x4>& worldScratch);

    // Seed a fresh graph: one layer with an "Idle" clip state (default) + a float "Speed"
    // parameter. Free + pure so the New-Asset seed is unit-tested without a live host.
    void SeedDefaultAnimationGraph(animation::AnimationGraphAsset& asset);

    // Registers the AnimationGraph page factory + an "Animation Graph" New-Asset creator.
    void RegisterAnimationGraphEditor(EditorContext& context, runtime::IApplicationHost& host,
                                      ui::runtime::UIHost& uiHost);
}
