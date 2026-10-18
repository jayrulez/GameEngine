// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Particles - :page partition.
//
// ParticleEffectEditorPage: a full authoring tool for a
// ParticleEffectAsset, modelled on (and exceeding) Sedulous's three-pane particle editor.
//
//   +-----------------+-------------------------------+---------------------+
//   |  authoring tree |  live preview  [transport]    |   node inspector    |
//   |  Effect         |   (the effect plays here)     |  (selected node's   |
//   |   +-System 0    |   speed---                    |   full properties)  |
//   |   |  +-Emitter  |   [stats overlay: alive/sys]  |                     |
//   |   |  +-Init...   |   [emission-shape gizmo]      |                     |
//   |   |  +-Behav...  |                               |                     |
//   +-----------------+-------------------------------+---------------------+
//
// The tree drives selection; the inspector shows only the selected node's fields. Edits mutate the
// authored ParticleEffect in place (the running preview picks them up immediately) and record a
// blob-snapshot undo command; structural edits (add/remove/reorder module or system) defer the tree
// + inspector rebuild through the UI mutation queue (never tear down the control that raised the
// event) and reselect the target by identity. Save writes the asset + requests a re-cook.
//
// Beyond Sedulous: real undo/redo (Sedulous's command stack was unused), a live particle-count stats
// overlay, an emission-shape gizmo for the selected system, module drag/menu reorder, a simulation
// speed slider, and pause.

module;
#include "Core/Prelude.h"

export module editor.particles:page;

import foundation.core;
import foundation.content;
import foundation.rhi;
import foundation.graphics;
import foundation.shell;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.particles;
import particles.pipeline;
import engine.particles;
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
    namespace particles = foundation::particles;

    // ---- Authoring-tree node model ------------------------------------------------------------
    // A flat table of nodes (nodeId = index) rebuilt from the effect on every structural change.
    // Each node points back to a concrete authoring target by (kind, systemIndex, moduleIndex) so
    // selection survives rebuilds via reselect-by-identity (node ids are otherwise invalidated).
    enum class ParticleNodeKind : u8
    {
        Effect,
        System,
        Emitter,
        InitializersFolder,
        BehaviorsFolder,
        Initializer,
        Behavior
    };

    struct ParticleTreeNode
    {
        ParticleNodeKind kind = ParticleNodeKind::Effect;
        i32 systemIndex = -1; // owning system (-1 for the Effect root)
        i32 moduleIndex = -1; // initializer/behavior index within the system
        i32 depth = 0;
        String label;
        Array<i32> children;
    };

    // A stable identity for a node, independent of nodeId (used to reselect after a rebuild).
    struct ParticleNodeRef
    {
        ParticleNodeKind kind = ParticleNodeKind::Effect;
        i32 systemIndex = -1;
        i32 moduleIndex = -1;

        [[nodiscard]] bool operator==(const ParticleNodeRef& o) const noexcept
        {
            return kind == o.kind && systemIndex == o.systemIndex && moduleIndex == o.moduleIndex;
        }
    };

    class ParticleTreeAdapter; // defined below (page holds it by UniquePtr)

    class ParticleEffectEditorPage final : public app::UIEditorPage, public IPlaybackPage
    {
    public:
        // ---- IPlaybackPage: play, pause and stop drive the preview's instance ----
        [[nodiscard]] bool CanPlay() const override;
        [[nodiscard]] bool IsPlaying() const override { return !m_paused && !m_stopped; }
        void Play() override;
        void Pause() override { SetPaused(true); }
        void Stop() override;
        void Restart() override;

        ParticleEffectEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                                 ui::runtime::UIHost& uiHost,
                                 foundation::content::Instance& instance);

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        [[nodiscard]] Status Save() override;

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        // Record a coalesced undo step for an in-place scalar edit that already happened (merge by
        // key). Public so the free-function inspector row helpers can commit through the page.
        void CommitEdit(StringView mergeKey);

    private:
        friend class ParticleTreeAdapter;

        // Whole-effect snapshot command: before/after blobs + a merge key (consecutive scrubs of the
        // same field collapse; the first command keeps the original `before`). ApplyEffectBlob is a
        // no-op when the live effect already equals the target, so Execute() at push time (state
        // already applied live) does nothing - only a real undo/redo re-deserializes + rebuilds.
        class EditParticleCommand final : public IEditorCommand
        {
        public:
            EditParticleCommand(ParticleEffectEditorPage& page, StringView mergeKey,
                                Array<byte> before, Array<byte> after)
                : m_page(&page), m_mergeKey(mergeKey), m_before(Move(before)), m_after(Move(after))
            {
            }
            [[nodiscard]] bool Execute() override
            {
                m_page->ApplyEffectBlob(m_after);
                return true;
            }
            void Undo() override { m_page->ApplyEffectBlob(m_before); }
            [[nodiscard]] StringView TypeId() const override { return u8"edit_particle"; }
            [[nodiscard]] bool MergeInto(IEditorCommand& previous) override
            {
                auto& prev = static_cast<EditParticleCommand&>(previous);
                if (prev.m_page != m_page || prev.m_mergeKey.AsView() != m_mergeKey.AsView())
                {
                    return false;
                }
                prev.m_after = Move(m_after);
                return true;
            }

        private:
            ParticleEffectEditorPage* m_page;
            String m_mergeKey;
            Array<byte> m_before;
            Array<byte> m_after;
        };

        // --- preview scene ---
        void BuildPreviewScene();
        [[nodiscard]] engine::particles::ParticleEffectComponent* PreviewComponent() const;
        // Resolve the authored effect's per-system mesh/texture/material refs into an in-memory resource
        // and attach it to the preview component for render-lookup (the SIM keeps the live borrowed
        // effect). Called after the scene builds + whenever a ref changes (picker) or the system set
        // changes (structural). Held as RefPtr<Object> to keep the interface free of the resource import.
        void RebuildPreviewResources();

        void SetPaused(bool paused);

        // --- tree ---
        void RebuildTree();                          // rebuild node table + refresh the view
        void SelectNode(const ParticleNodeRef& ref); // select + rebuild inspector
        [[nodiscard]] i32 NodeIdForRef(const ParticleNodeRef& ref) const;
        [[nodiscard]] particles::ParticleSystem* SelectedSystem() const;
        void ShowNodeContextMenu(i32 nodeId, f32 screenX, f32 screenY);
        // Deferred (mutation-queue) structural apply: run `mutate`, snapshot undo, then rebuild the
        // tree + inspector and reselect `reselect`. Never called from inside a live event unqueued.
        void QueueStructural(StringView undoKey, Function<void()> mutate, ParticleNodeRef reselect);

        // --- inspector ---
        void RebuildInspector();
        // Defer a RebuildInspector to the mutation queue (safe to call mid-event-dispatch: it destroys
        // the property-grid views, so it must not run while the grid is dispatching). Used by the enum
        // rows whose selection changes which sections/fields are visible (render mode, emission mode).
        void QueueInspectorRebuild();
        // Add a system (the tree header's icon, the effect menu) or delete one (the system menu,
        // its General section's icon).
        void AddSystem();
        void DeleteSystem(i32 sysIndex);
        // One write for a system's material slot, whether the material came from a pick or a drop.
        void AssignMaterial(i32 sysIndex, usize slot, const Guid& picked);
        void BuildEffectInspector();
        void BuildSystemInspector(particles::ParticleSystem& sys);
        void BuildEmitterInspector(particles::ParticleSystem& sys);
        void BuildModuleInspector(ISerializable* module);

        // Draw the selected system's emission-shape gizmo into the preview (per-scene debug draw).
        void DrawEmissionGizmo();

        // --- undo ---
        [[nodiscard]] Array<byte> SnapshotEffect() const;
        void ApplyEffectBlob(const Array<byte>& blob); // deserialize + reattach + defer rebuild

        [[nodiscard]] ui::UIContext* Ctx() const;

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        RefPtr<pipeline::ParticleEffectAsset> m_asset;

        // preview world (shared substrate: viewport + preview scene + camera + render loop)
        UniquePtr<PreviewViewport> m_preview;
        RefPtr<Object> m_previewResource; // resolved ParticleEffectResource for the preview (owned)
        scene::EntityHandle m_emitter; // the emitter entity (in m_preview->Scene())

        // views
        RefPtr<ui::toolkit::DraggableTreeView> m_tree;
        UniquePtr<ParticleTreeAdapter> m_adapter;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<ui::Label> m_statsLabel; // live overlay text
        RefPtr<ui::Label> m_titleLabel; // inspector header ("Systems: N")
        RefPtr<foundation::ui::View> m_content;

        // tree state
        Array<ParticleTreeNode> m_nodes;
        Array<i32> m_roots;
        ParticleNodeRef m_selected; // currently-inspected node

        // transport / undo state
        f32 m_simSpeed = 1.0f;
        bool m_paused = false;
        bool m_stopped = false; // stopped by the toolbar: nothing emits until Play or Restart
        RefPtr<app::PageToolbar> m_toolbar;
        Array<byte> m_undoBaseline; // last committed effect blob (undo anchor)
    };

    // Tree adapter over the page's node table (Effect -> System -> {Emitter, folders} -> modules).
    // Reorder is constrained to same-parent module siblings and routed to Move{Initializer,Behavior}.
    class ParticleTreeAdapter final : public ui::toolkit::IReorderableTreeAdapter
    {
    public:
        explicit ParticleTreeAdapter(ParticleEffectEditorPage& owner) : m_owner(&owner) {}

        [[nodiscard]] i32 RootCount() const override;
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override;
        [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override;
        [[nodiscard]] i32 GetDepth(i32 nodeId) const override;
        [[nodiscard]] bool HasChildren(i32 nodeId) const override;
        [[nodiscard]] RefPtr<ui::View> CreateView(i32 viewType) override;
        void BindView(ui::View* view, i32 nodeId, i32 depth, bool isExpanded) override;

        [[nodiscard]] bool CanMove(i32 fromPosition, i32 toPosition) override;
        void MoveItem(i32 fromPosition, i32 toPosition) override;

    private:
        ParticleEffectEditorPage* m_owner;
    };

    class ParticleEffectPageFactory final : public IEditorPageFactory
    {
    public:
        ParticleEffectPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
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


    // Registers the ParticleEffect page factory + a "Particle Effect" New-Asset creator.
    void RegisterParticleEditor(EditorContext& context, runtime::IApplicationHost& host,
                                ui::runtime::UIHost& uiHost);
}
