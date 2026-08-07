// Draconic::EditorScene - :mesh_page partition.
//
// MeshEditorPage (editor-pages-gap.md, bespoke pass #2): the mesh viewer. Opens a
// StaticMeshAsset or SkinnedMeshAsset with a GPU orbit preview of the COOKED mesh product on
// the left (bound by the asset's guid through the editor's cooked-DB resources, lit by a
// default sun + procedural sky, shown under a neutral PBR material) and a stats readout on the
// right - vertex/index/submesh counts, bounds, skinning, and a per-submesh breakdown. This
// closes the hard "No editor registered" failure for the two most common asset types.
//
// It is a VIEWER: mesh assets carry no re-authorable fields (a mesh's per-submesh material
// INDICES are baked into the cooked source at import; material BINDINGS live on a scene's
// MeshComponent, not on the asset), so Save is a no-op. The preview watches the bound product
// and re-frames + refreshes stats on cook / hot-reload.
//
// TODO(editor-pages-gap #2): a page-local preview-material picker per submesh slot (persisted
// like MaterialPage's preview-mesh pref) + a "create entity / save-as-prefab" action.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.editor.scene:mesh_page;

import draconic.foundation;
import draconic.content;
import draconic.rhi;
import draconic.graphics;
import draconic.shell;
import draconic.runtime;
import draconic.runtime.client;
import draconic.scene;
import draconic.engine.scene;
import draconic.geometry;
import draconic.materials;
import draconic.resource;
import draconic.render;
import draconic.engine.render;
import draconic.ui;
import draconic.ui.runtime;
import draconic.ui.viewport;
import draconic.vg.renderer;
import draconic.editor.core;
import draconic.editor.app;
import :camera; // EditorCamera (orbit/fly camera on the preview viewport)

using namespace draconic::foundation;

export namespace draconic::editor
{
    namespace runtime = draconic::runtime;
    namespace ui = draconic::ui;
    namespace vg = draconic::vg;
    namespace scene = draconic::scene;
    namespace render = draconic::render;
    namespace geometry = draconic::geometry;
    namespace materials = draconic::materials;
    namespace resource = draconic::resource;

    class MeshEditorPage final : public app::UIEditorPage
    {
    public:
        MeshEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                       ui::runtime::UIHost& uiHost, draconic::content::Instance& instance);

        [[nodiscard]] draconic::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }

        // Viewer: a mesh asset has no re-authorable fields (see the module comment).
        [[nodiscard]] Status Save() override { return Status{}; }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            draconic::graphics::FrameContext& frame) override;
        void OnClose() override;

    private:
        // Preview world: one entity with a MeshComponent + a seeded sun (the render subsystem
        // injects the default environment / procedural sky on CreateScene).
        void BuildPreviewScene();

        // (Re)bind the cooked mesh product by the asset's guid and point the MeshComponent at
        // it; assigns the neutral default material. Safe before the product is cooked (no-op).
        void BindMesh();

        // Point the preview entity's MeshComponent at `mesh` (null = clear, not cooked yet).
        void PointComponentAtMesh(geometry::StaticMesh* mesh);

        // Rebuild the stats labels from the live mesh (name / counts / bounds / submeshes).
        void RefreshStats();
        void AddStatLine(StringView text);

        // Reframe the orbit camera to fit the mesh bounds.
        void FramePreview(const geometry::StaticMesh* mesh);

        void EnsureViewportBound();

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        scene::SceneSubsystem* m_scenes = nullptr;
        scene::SceneManager m_sceneManager; // this page's OWN preview scene group
        render::RenderSubsystem* m_render = nullptr;
        scene::Scene* m_scene = nullptr;
        scene::EntityHandle m_entity;
        RefPtr<materials::Material> m_defaultMaterial;

        resource::Proxy<geometry::StaticMesh> m_meshProxy; // the cooked product (follows reloads)
        u64 m_lastUid = 0;                                 // product identity - detects hot-reload

        EditorCamera m_camera;
        UniquePtr<draconic::shell::InputRouter> m_router;

        RefPtr<ui::viewport::ViewportView> m_viewport;
        RefPtr<draconic::ui::FlexLayout> m_statsColumn; // one Label per stat line
        RefPtr<draconic::ui::View> m_content;
        draconic::graphics::RenderWindow* m_hostWindow = nullptr;
    };

    class MeshEditorPageFactory final : public IEditorPageFactory
    {
    public:
        MeshEditorPageFactory(const TypeInfo& type, runtime::IApplicationHost& host,
                              ui::runtime::UIHost& uiHost)
            : m_type(&type), m_host(&host), m_uiHost(&uiHost)
        {
        }

        [[nodiscard]] const TypeInfo* PrimaryType() const override { return m_type; }
        [[nodiscard]] UniquePtr<EditorPage>
        CreatePage(EditorContext& context, draconic::content::Instance& instance) override;

    private:
        const TypeInfo* m_type;
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Human-readable stat lines for a cooked mesh (name / counts / bounds / skinning / one line
    // per submesh) - the viewer's readout. Free + pure so it is unit-tested without a live host.
    [[nodiscard]] Array<String> MeshStatLines(const geometry::StaticMesh& mesh);

    // Registers the viewer for BOTH mesh asset types (they are sibling editor::Asset subclasses,
    // so a factory each - not one via nearest-type dispatch).
    void RegisterMeshEditor(EditorContext& context, runtime::IApplicationHost& host,
                            ui::runtime::UIHost& uiHost);
}
