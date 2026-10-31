// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Vegetation - the `:mask_page` partition.
//
// VegetationMaskPage: a vegetation mask on the GUI, built from the image pages' markup
// (Data/Editor/ImagePage.xml). A mask says where each vegetation layer that places by Mask may
// grow, with one density plane per layer. The page shows one plane at a time, chosen on the
// picture's bar where the channel choice would be (a plane is a single value, not colours), from
// none to full as black to white, with the density under the pointer read out. The mask's options
// sit beside it on the grid, as the heightfield page shows its heights. Painting is a tool of the
// scene view.
//
// The size and plane count follow what the cook reads. A painted mask's planes are stored at its
// size, so once anything is painted its width and height are read-only (a notice says why). Its
// plane count may change: the cook keeps the planes that still exist and adds new ones empty. An
// imported mask is decoded from its image at the image's size, one plane per channel (red, green,
// blue, alpha), so its options are read-only and its size is a fact.

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
        static constexpr StringView kPath = u8"Editor/ImagePage.xml";

        VegetationMaskPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);

        /// Shows plane `plane` (0-based), as choosing it on the picture's bar does.
        void ShowPlane(u32 plane);
        /// The density at (x, y) of the plane shown, 0 to 255; false outside the mask or when
        /// nothing was read.
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
        // Reads the planes as the cook would: from the painted sidecar, or from the imported image.
        void ReadPlanes();
        void ShowPlanePicture();
        void ShowChoices();
        void ShowFacts();

        UniquePtr<app::ImagePreview> m_preview;
        RefPtr<foundation::gui::ComboBox> m_planeChoice;
        Array<u8> m_densities; // all planes, one after another, each row by row
        i32 m_width = 0;
        i32 m_height = 0;
        u32 m_planeCount = 0; // the number of planes read (four for an imported mask)
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

    /// Registers the vegetation mask page, which asks for its services when it is created.
    void RegisterVegetationMaskEditor(EditorContext& context, const app::AssetPageServicesSource& services);
}
