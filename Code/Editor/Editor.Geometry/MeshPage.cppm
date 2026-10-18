// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :page partition.
//
// MeshEditorPage: the mesh viewer. Opens a
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
// TODO: a page-local preview-material picker per submesh slot (persisted
// like MaterialPage's preview-mesh pref) + a "create entity / save-as-prefab" action.

module;
#include "Core/Prelude.h"

export module editor.geometry:page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import engine.scene;
import foundation.geometry;
import foundation.materials;
import foundation.resource;
import foundation.render;
import engine.render;
import foundation.ui;
import foundation.ui.runtime;
import editor.core;
import editor.app;
import editor.preview; // PreviewViewport (shared viewport + preview scene + camera + render loop)

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace ui = foundation::ui;
    namespace scene = foundation::scene;
    namespace render = foundation::render;
    namespace geometry = foundation::geometry;
    namespace materials = foundation::materials;
    namespace resource = foundation::resource;

    class MeshEditorPage final : public app::UIEditorPage
    {
    public:
        MeshEditorPage(EditorContext& context, runtime::IApplicationHost& host,
                       ui::runtime::UIHost& uiHost, foundation::content::Instance& instance);

        [[nodiscard]] foundation::ui::View* ContentView() override { return m_content.Get(); }
        [[nodiscard]] StringView Title() const override { return m_title.AsView(); }

        // Viewer: a mesh asset has no re-authorable fields (see the module comment).
        [[nodiscard]] Status Save() override { return Status{}; }

        void OnUpdate(runtime::IApplicationHost&, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&,
                            foundation::graphics::FrameContext& frame) override;
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

        // Preview material: pick a MaterialAsset to render the preview with (else the neutral
        // default). ApplyPreviewMaterial pushes the current choice onto the MeshComponent.
        // The preview's material, nil for the default; persisted as a preview preference.
        void SetPreviewMaterial(const Guid& picked);
        void ApplyPreviewMaterial();
        // Push the LOD row's choice onto the preview MeshComponent's forceLod knob.
        void ApplyPreviewLod();

        // Persist / restore the preview-material choice per asset (per-project editor settings).
        void LoadPreviewPref();
        void SavePreviewPref();

        // Rebuild the stats labels from the live mesh (name / counts / bounds / submeshes).
        void RefreshStats();
        void AddStatLine(StringView text);

        // Reframe the orbit camera to fit the mesh bounds.
        void FramePreview(const geometry::StaticMesh* mesh);

        EditorContext* m_context = nullptr;
        runtime::IApplicationHost* m_host = nullptr;
        ui::runtime::UIHost* m_uiHost = nullptr;
        String m_title;

        UniquePtr<PreviewViewport> m_preview;        // shared viewport + preview scene + camera
        scene::EntityHandle m_entity;                // the preview mesh entity (in m_preview->Scene())
        RefPtr<materials::Material> m_defaultMaterial;
        resource::Proxy<materials::Material> m_previewMaterial; // chosen override (null = default)
        Guid m_previewMaterialId;                               // its source guid (for the label)
        i32 m_previewForceLod = -1; // the LOD row's choice (-1 = auto), pushed to forceLod
        RefPtr<app::ResourceRefEditor> m_previewMaterialRow;   // the stats column's material slot

        resource::Proxy<geometry::StaticMesh> m_meshProxy; // the cooked product (follows reloads)
        u64 m_lastUid = 0;                                 // product identity - detects hot-reload

        RefPtr<foundation::ui::FlexLayout> m_statsColumn; // one Label per stat line
        RefPtr<foundation::ui::View> m_content;
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
        CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const TypeInfo* m_type;
        runtime::IApplicationHost* m_host;
        ui::runtime::UIHost* m_uiHost;
    };

    // Human-readable stat lines for a cooked mesh (name / counts / bounds / skinning / one line
    // per submesh) - the viewer's readout. Free + pure so it is unit-tested without a live host.
    [[nodiscard]] Array<String> MeshStatLines(const geometry::StaticMesh& mesh);

    // Registers the viewer for BOTH mesh asset types (they are sibling pipeline::Asset subclasses,
    // so a factory each - not one via nearest-type dispatch).
    void RegisterMeshEditor(EditorContext& context, runtime::IApplicationHost& host,
                            ui::runtime::UIHost& uiHost);
}
