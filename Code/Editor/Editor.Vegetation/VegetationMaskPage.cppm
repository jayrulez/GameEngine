// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - the `:mask_page` partition.
//
// VegetationMaskPage: a vegetation mask on the GUI from the image pages' markup
// (Data/Editor/ImagePage.gml; Documentation/Specs/editor-gui.md, 5.4, layer 7, part 2), new: no
// editor had one, a mask opened on the generic page. A mask is where each vegetation layer that
// places by Mask may grow, a density plane per layer; the page shows one plane at a time (chosen
// on the picture's bar, in the channel's place: a plane is one value, not colours), none to full
// as black to white, the pointer's density read out, beside the mask's options on the grid, as the
// heightfield page shows its heights. Painting stays the scene view's tool.
//
// Its size and plane count, as the cook reads them: a painted mask's planes are kept at its size,
// so once anything is painted its width and height are read only (a notice says why); its plane
// count may change (the cook keeps the planes that still exist, a new one empty). An imported mask
// is decoded from its image at its own size, a plane per channel (red, green, blue, alpha): its
// options are read only, its size a fact.

module;
#include "Core/Prelude.h"

export module editor.vegetation:mask_page;

import foundation.core;
import foundation.content;
import foundation.gui;
import vegetation.pipeline; // VegetationMaskAsset
import editor.core;
import editor.app;

using namespace foundation::core;

export namespace editor
{
    class VegetationMaskPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/ImagePage.gml";

        VegetationMaskPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        /// Shows plane `plane` (from 0), as its choice on the picture's bar does.
        void ShowPlane(u32 plane);
        /// The density at (x, y) of the plane shown, 0 to 255; false off the mask or with nothing read.
        bool DensityAt(i32 x, i32 y, u8& out) const;

        // === For tests ===
        [[nodiscard]] u32 ShownPlane() const noexcept { return m_plane; }
        [[nodiscard]] u32 PlaneCount() const noexcept { return m_planeCount; }
        [[nodiscard]] bool Painted() const noexcept { return m_painted; }
        [[nodiscard]] bool Imported() const noexcept { return m_imported; }
        [[nodiscard]] foundation::gui::ComboBox* PlaneChoice() const noexcept { return m_planeChoice.Get(); }
        [[nodiscard]] app::ImagePreview* Preview() const noexcept { return m_preview.Get(); }

    protected:
        void OnObjectChanged() override;

    private:
        // The planes as the cook would read them: the painted sidecar, or the imported image.
        void ReadPlanes();
        void ShowPlanePicture();
        void ShowChoices();
        void ShowFacts();

        UniquePtr<app::ImagePreview> m_preview;
        RefPtr<foundation::gui::ComboBox> m_planeChoice;
        Array<u8> m_densities; // every plane, plane after plane, row by row
        i32 m_width = 0;
        i32 m_height = 0;
        u32 m_planeCount = 0; // the planes read (an imported mask's four, a painted one's)
        u32 m_plane = 0;
        bool m_painted = false;
        bool m_imported = false;
    };

    class VegetationMaskPageFactory final : public IEditorPageFactory
    {
    public:
        explicit VegetationMaskPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };

    /// The vegetation mask page (asking for its services when it is made).
    void RegisterVegetationMaskEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
