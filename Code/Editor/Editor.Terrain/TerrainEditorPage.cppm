// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Terrain - the `:page` partition.
//
// TerrainEditorPage: a terrain on the GUI from the preview pages' markup (Data/Editor/PreviewPage.xml;
// Documentation/Specs/editor-gui.md, 5.4, layer 7). The cooked terrain on a preview lit by a sun
// (its self-shadowing shown), beside its rows. The asset keeps its layers in parallel arrays, so
// the rows are the page's own, not its reflection's: under Terrain the heightfield and the splat
// weights it is painted with, whether it casts shadows, how soft the height blend is and the size
// every layer's textures are packed at; under Base layer what shows wherever no paint covers (its
// albedo, normal, ORM and height maps and its tiling); under Paint layers a list, each layer a
// group of its maps (albedo, normal, ORM, height, coverage mask) and its tiling. A layer's place in
// the list is its channel in the splat weights, so the list adds and removes but does not reorder;
// a layer removed frees its weights and moves those above it down. A terrain with no splat weights
// gets new ones from the bar's New Splat Weights (the paint tool only edits weights that exist).
// What the cooked terrain is (its grid, world size, chunks) are read-only rows under Source.
//
// Each edit is one undo step (the whole asset before and after: it is small). Save writes it and
// asks for a cook, after which the preview follows the new product. Brushes never live here:
// sculpting and painting are the scene view's tools.

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
import foundation.terrain.resource; // TerrainResource (the preview's product, the facts)
import terrain.pipeline;            // TerrainAsset
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    namespace terrain = foundation::terrain;

    /// What a cooked terrain is, as the page's read-only rows say it: each a name and its value.
    struct TerrainFact
    {
        String name;
        String value;
    };
    /// The facts of `product` (its grid, world size, chunks, layers, shadows). Pure, so tested
    /// without a host.
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

        /// The rows' sections: the terrain's, its base layer's, its paint layers'.
        [[nodiscard]] foundation::gui::PropertyBag& TerrainRows() noexcept { return *m_terrainRows; }
        [[nodiscard]] foundation::gui::PropertyBag& BaseRows() noexcept { return *m_baseRows; }
        [[nodiscard]] foundation::gui::PropertyBag& LayerRows() noexcept { return *m_layerRows; }
        /// Writes `value` to row `path` of `rows` as the grid commits an edit (one undo step).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        /// A list edit of the paint layers as the grid makes one (an add, a remove).
        void EditLayers(const foundation::gui::ListEdit& edit);
        /// New splat weights of `size` texels a side, made beside the terrain and given to it (one
        /// undo step for the terrain; the new asset stays). False when it has some already.
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
        const void* m_shownProduct = nullptr;                            // what the preview and facts show
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

    /// The terrain page (asking for its services when it is made).
    void RegisterTerrainEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
