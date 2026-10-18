// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :skeleton_page partition.
//
// SkeletonEditorPage: a VIEWER for a SkeletonAsset - a bone
// TREE (hierarchy by parent index) beside a bind-pose wireframe viewport (the
// DrawSkeletonWireframe over the cooked product's local bind poses), plus a read-only info pane for
// the selected bone (index / parent / bind TRS). Selecting a bone emphasizes it in the wireframe.
// Skeletons are imported (model importer) and carry no re-authorable fields, so Save is a no-op and
// there is no creator. The page watches the bound product (pointer identity) so a late cook or
// hot-reload refreshes the tree + preview.

module;
#include "Core/Prelude.h"

export module editor.scene:skeleton_page;

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

    class SkeletonTreeAdapter; // defined below

    class SkeletonEditorPage final : public app::UIEditorPage
    {
    public:
        SkeletonEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                           ui::runtime::UIHost& uiHost, foundation::content::Instance& instance);

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }
        // Viewer: skeletons carry no re-authorable fields (imported), so Save is a no-op.
        [[nodiscard]] Status Save() override { return Status{}; }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

    private:
        friend class SkeletonTreeAdapter;

        void RebuildTree();     // node table from the cooked skeleton's hierarchy
        void RebuildInfoPane(); // read-only rows for the selected bone
        void UpdatePreview();   // bind-pose wireframe + selected-bone emphasis

        [[nodiscard]] ui::UIContext* Ctx() const;

        struct BoneNode
        {
            i32 boneIndex = -1;
            i32 depth = 0;
            Array<i32> children; // node ids
        };

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        foundation::resource::Proxy<animation::Skeleton> m_skeleton; // cooked product
        animation::Skeleton* m_lastSkeleton = nullptr;             // watchdog (identity)

        // preview world (debug-draw only: shared substrate hosts a scene we draw wireframe into)
        UniquePtr<PreviewViewport> m_preview;

        RefPtr<ui::toolkit::DraggableTreeView> m_tree;
        UniquePtr<SkeletonTreeAdapter> m_adapter;
        RefPtr<ui::toolkit::PropertyGrid> m_grid;
        RefPtr<ui::Label> m_statsLabel;
        RefPtr<foundation::ui::View> m_content;

        Array<BoneNode> m_nodes; // nodeId = index (bone order)
        Array<i32> m_roots;
        i32 m_selectedBone = -1;
        Array<animation::BoneTransform> m_poseScratch;
        Array<Float4x4> m_worldScratch;
    };

    // Read-only tree over the page's bone-node table (no reorder - the hierarchy is imported).
    class SkeletonTreeAdapter final : public ui::toolkit::IReorderableTreeAdapter
    {
    public:
        explicit SkeletonTreeAdapter(SkeletonEditorPage& owner) : m_owner(&owner) {}

        [[nodiscard]] i32 RootCount() const override;
        [[nodiscard]] i32 GetChildCount(i32 nodeId) const override;
        [[nodiscard]] i32 GetChildId(i32 parentId, i32 childIndex) const override;
        [[nodiscard]] i32 GetDepth(i32 nodeId) const override;
        [[nodiscard]] bool HasChildren(i32 nodeId) const override;
        [[nodiscard]] RefPtr<ui::View> CreateView(i32 viewType) override;
        void BindView(ui::View* view, i32 nodeId, i32 depth, bool isExpanded) override;

        [[nodiscard]] bool CanMove(i32, i32) override { return false; }
        void MoveItem(i32, i32) override {}

    private:
        SkeletonEditorPage* m_owner;
    };

    class SkeletonPageFactory final : public IEditorPageFactory
    {
    public:
        SkeletonPageFactory(runtime::IApplicationHost& host, ui::runtime::UIHost& uiHost)
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

    // Pure stats readout (name, bones, roots, max depth) - headless-testable like MeshStatLines.
    [[nodiscard]] Array<String> SkeletonStatLines(const animation::Skeleton& skeleton);

    // Registers the Skeleton page factory (no creator - skeletons come from import).
    void RegisterSkeletonEditor(EditorContext& context, runtime::IApplicationHost& host,
                                ui::runtime::UIHost& uiHost);
}
