// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Geometry - :page partition.
//
// MeshEditorPage: the mesh viewer. Opens a StaticMeshAsset or SkinnedMeshAsset with a GPU orbit
// preview of the COOKED mesh product (bound by the asset's guid through the editor's cooked-DB
// resources, lit by a default sun + procedural sky, shown under a neutral PBR material) beside its
// stats, read only: vertex/index/submesh counts, bounds, skinning, a per-submesh breakdown and the
// LOD chain. On the GUI from the preview pages' markup (Data/Editor/PreviewPage.xml;
// Documentation/Specs/editor-gui.md, 5.4, layer 3): the preview's own choices on its bar, where
// they had been among the stats (the catalog's inconsistency 10): the material it shows the mesh in
// (kept per asset) and, for a chain, the level of detail it draws. A mesh not cooked yet says so
// in the preview's place.
//
// It is a VIEWER: mesh assets carry no re-authorable fields (a mesh's per-submesh material
// INDICES are baked into the cooked source at import; material BINDINGS live on a scene's
// MeshComponent, not on the asset). The preview watches the bound product and re-frames and
// refreshes its stats on cook / hot-reload.

module;
#include "Core/Prelude.h"

export module editor.geometry:page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.scene;
import foundation.geometry;
import foundation.materials;
import foundation.resource;
import foundation.gui;
import editor.core;
import editor.app;
import editor.preview;

using namespace foundation::core;

export namespace editor
{
    namespace runtime = foundation::runtime;
    namespace scene = foundation::scene;
    namespace geometry = foundation::geometry;
    namespace materials = foundation::materials;
    namespace resource = foundation::resource;

    class MeshEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/PreviewPage.xml";

        MeshEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance, const TypeInfo& type);

        void OnUpdate(runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// The preview's material (nil: the neutral default), kept with the project per asset.
        void SetPreviewMaterial(const Guid& picked);
        [[nodiscard]] const Guid& PreviewMaterial() const noexcept { return m_previewMaterialId; }
        /// The level the preview draws (-1: the one the game would choose).
        void SetPreviewLod(i32 lod);

        // === For tests ===
        [[nodiscard]] app::PreviewPane* Pane() const noexcept { return m_pane.Get(); }
        [[nodiscard]] app::AssetPickerSlot* MaterialSlot() const noexcept { return m_materialSlot.Get(); }
        [[nodiscard]] foundation::gui::ComboBox* LodChoice() const noexcept { return m_lod.Get(); }

    private:
        // Preview world: one entity with a MeshComponent + a seeded sun (the render subsystem
        // injects the default environment / procedural sky on CreateScene).
        void BuildPreviewScene();
        // (Re)bind the cooked mesh product by the asset's guid; safe before it is cooked (no-op).
        void BindMesh();
        // Point the preview entity's MeshComponent at `mesh` (null = clear, not cooked yet).
        void PointComponentAtMesh(geometry::StaticMesh* mesh);
        void ApplyPreviewMaterial();
        void ApplyPreviewLod();
        void ShowMaterial();
        void LoadPreviewPref();
        void SavePreviewPref();
        // The stats as facts, the LOD choice as the chain is, and the preview or why there is none.
        void ShowMesh(geometry::StaticMesh* mesh);
        // Reframe the orbit camera to fit the mesh bounds.
        void FramePreview();

        UniquePtr<app::PreviewPane> m_pane;
        RefPtr<app::AssetPickerSlot> m_materialSlot; // the preview bar's
        RefPtr<foundation::gui::ComboBox> m_lod;     // the preview bar's, a chain's levels
        scene::EntityHandle m_entity;                // the preview mesh entity
        RefPtr<materials::Material> m_defaultMaterial;
        resource::Proxy<materials::Material> m_previewMaterial; // chosen override (null = default)
        Guid m_previewMaterialId;
        i32 m_previewForceLod = -1; // -1 = auto, pushed to forceLod
        resource::Proxy<geometry::StaticMesh> m_meshProxy; // the cooked product (follows reloads)
        u64 m_lastUid = 0;                                 // product identity - detects hot-reload
    };

    class MeshEditorPageFactory final : public IEditorPageFactory
    {
    public:
        MeshEditorPageFactory(const TypeInfo& type, const app::AssetPageServicesSource& services) : m_type(&type), m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return m_type; }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const TypeInfo* m_type;
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    // Human-readable stat lines for a cooked mesh (name / counts / bounds / skinning / one line
    // per submesh / the LOD chain): "Key: value", a submesh's and a level's indented. Free and
    // pure so it is unit-tested without a live host; the page shows each as a read-only row.
    [[nodiscard]] Array<String> MeshStatLines(const geometry::StaticMesh& mesh);

    // Registers the viewer for BOTH mesh asset types (they are sibling pipeline::Asset subclasses,
    // so a factory each - not one via nearest-type dispatch), each page asking for its services
    // when it is made, and the preview prefs' section.
    void RegisterMeshEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
