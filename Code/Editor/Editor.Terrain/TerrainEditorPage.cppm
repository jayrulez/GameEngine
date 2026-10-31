// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `:page` partition.
//
// TerrainEditorPage: a terrain on the GUI, built from the preview pages' markup
// (Data/Editor/PreviewPage.xml). It shows the cooked terrain in a preview lit by a sun (so its
// self-shadowing shows), beside its rows. The asset keeps its layers in parallel arrays, so the
// page builds its own rows instead of using reflection:
// - Terrain: the heightfield and the splat weights it is painted with, whether it casts shadows,
//   how soft the height blend is and the size every layer's textures are packed at.
// - Base layer: what shows wherever no paint covers (its albedo, normal, ORM and height maps and
//   its tiling).
// - Paint layers: a list, each layer a group of its maps (albedo, normal, ORM, height, coverage
//   mask) and its tiling.
// - Source: read-only facts about the cooked terrain (its grid, world size, chunks).
// A layer's position in the list is its channel in the splat weights, so the list can add and
// remove but not reorder; removing a layer frees its weights and moves the layers above it down.
// A terrain with no splat weights gets new ones from the bar's New Splat Weights, since the paint
// tool only edits weights that exist.
//
// Each edit is one undo step holding the whole asset before and after, since it is small. Save
// writes it and asks for a cook, after which the preview follows the new product. There are no
// brushes here: sculpting and painting are tools of the scene view.

module;
#include "Core/Prelude.h"

export module editor.terrain:page;

import foundation.core;
import foundation.content;
import foundation.graphics;
import foundation.runtime;
import foundation.runtime.client;
import foundation.resource;
import foundation.scene;
import foundation.gui;
import foundation.gui.toolkit;
import foundation.terrain.resource; // TerrainResource (the preview's product and the facts)
import terrain.pipeline;            // TerrainAsset
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace terrain = foundation::terrain;

    /// One fact about a cooked terrain, as a read-only row shows it: a name and its value.
    struct TerrainFact
    {
        String name;
        String value;
    };
    /// The facts of `product` (its grid, world size, chunks, layers, shadows). Pure, so it can be
    /// tested without a host.
    [[nodiscard]] Array<TerrainFact> TerrainFacts(const terrain::TerrainResource& product);

    class TerrainEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/PreviewPage.xml";
        /// The sizes New Splat Weights offers, in its menu's order.
        static constexpr i32 kSplatSizes[] = {512, 1024, 2048};

        TerrainEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~TerrainEditorPage() override;

        void OnUpdate(foundation::runtime::IApplicationHost& host, f32 dt) override;
        void OnRenderWindow(foundation::runtime::IApplicationHost&, foundation::graphics::FrameContext& frame) override;
        void OnClose() override;

        /// The row sections: the terrain, its base layer and its paint layers.
        [[nodiscard]] foundation::gui::PropertyBag& TerrainRows() noexcept { return *m_terrainRows; }
        [[nodiscard]] foundation::gui::PropertyBag& BaseRows() noexcept { return *m_baseRows; }
        [[nodiscard]] foundation::gui::PropertyBag& LayerRows() noexcept { return *m_layerRows; }
        /// Writes `value` to row `path` of `rows` as the grid commits an edit (one undo step).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        /// Applies a list edit of the paint layers as the grid makes one (an add or a remove).
        void EditLayers(const foundation::gui::ListEdit& edit);
        /// Creates splat weights of `size` texels a side beside the terrain and assigns them to it
        /// (one undo step for the terrain; the new asset is kept on undo). Returns false when the
        /// terrain already has splat weights.
        bool CreateSplatWeights(i32 size);

        // === For tests ===
        [[nodiscard]] const pipeline::TerrainAsset* Asset() const { return ObjectAs<pipeline::TerrainAsset>(); }
        [[nodiscard]] foundation::gui::ToolMenuButton* SplatButton() const noexcept { return m_splat.Get(); }

    protected:
        void OnObjectChanged() override;

    private:
        void BuildRows();
        void ShowRows();
        void ShowFacts();
        void AddSplatButton();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void RemapWeightsOnRemove(u32 removed);

        void BuildPreviewScene();
        void PointPreviewAt(terrain::TerrainResource* product);
        void FramePreview();

        UniquePtr<foundation::gui::PropertyBag> m_terrainRows;
        UniquePtr<foundation::gui::PropertyBag> m_baseRows;
        UniquePtr<foundation::gui::PropertyBag> m_layerRows;
        RefPtr<foundation::gui::ToolMenuButton> m_splat;
        UniquePtr<app::PreviewPane> m_pane;
        foundation::scene::EntityHandle m_entity;
        foundation::resource::Proxy<terrain::TerrainResource> m_product; // the cooked terrain (follows reloads)
        const void* m_shownProduct = nullptr;                            // the product the preview and facts show
    };

    class TerrainEditorPageFactory final : public IEditorPageFactory
    {
    public:
        explicit TerrainEditorPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return &pipeline::TerrainAsset::StaticType(); }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// Registers the terrain page, which asks for its services when it is created.
    void RegisterTerrainEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
