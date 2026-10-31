// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the `:bus_layout_page` partition.
//
// AudioBusLayoutEditorPage: the mixer's buses, built on the GUI from its markup
// (Data/Editor/BusLayoutPage.xml). The bus tree (the four fixed buses, Master with Effects, Music
// and UI under it, and the custom buses under their parents) sits beside the grid, which shows the
// chosen bus: a custom bus's name and parent (offering only parents that make no cycle), then its
// mix (volume, mute), its filters, its delay and its reverb (each off at 0). The tree's bar and a
// row's menu add a bus and remove a custom one. Each edit is one undo step. The rules the edits
// keep (each bus has a unique name, children follow a rename, and children move to Master when
// their parent goes) live in :bus_layout_model. A layout whose buses make a cycle, which the cook
// refuses, says so.

module;
#include "Core/Prelude.h"

export module editor.audio:bus_layout_page;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.audio;
import audio.pipeline;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import :bus_layout_model;

using namespace foundation::core;

export namespace editor
{
    class AudioBusLayoutEditorPage final : public app::AssetObjectPage
    {
    public:
        static constexpr StringView kPath = u8"Editor/BusLayoutPage.xml";

        AudioBusLayoutEditorPage(EditorContext& context, app::AssetPageServices services, foundation::content::Instance& instance);
        ~AudioBusLayoutEditorPage() override;

        /// The bus the grid shows (Master when the page opens).
        [[nodiscard]] const BusRef& Selected() const noexcept { return m_selected; }
        void Select(const BusRef& bus);
        /// Adds a bus under Master in the first free slot and chooses it; false when every slot is
        /// in use.
        bool AddBus();
        /// Removes the chosen bus (a custom one; its children move to Master) and chooses Master.
        bool RemoveSelected();

        // === For tests ===
        /// The tree's rows, indented by depth ("Master\n  Effects\n    Footsteps").
        [[nodiscard]] String Tree() const;
        [[nodiscard]] foundation::gui::PropertyBag& BusRows() noexcept { return *m_busRows; }
        /// The chosen bus's settings in one group: "Mix", "Filters", "Delay" or "Reverb".
        [[nodiscard]] foundation::gui::PropertyBag& EffectRows(StringView group);
        /// Edits a row as the grid commits one (`rows` is one of the page's bags).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);

    protected:
        void OnObjectChanged() override;

    private:
        [[nodiscard]] pipeline::AudioBusLayoutAsset* Layout() const { return ObjectAs<pipeline::AudioBusLayoutAsset>(); }
        void BuildRows();
        /// Builds the tree from the layout, with the chosen bus's row selected.
        void ShowTree();
        /// Shows the chosen bus's rows: its values, and its name and parent for a custom bus.
        void ShowBus();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void ShowMenu(i32 item, Float2 at);

        UniquePtr<foundation::gui::TreeModel> m_model;
        Array<BusRow> m_rows; // the tree's rows, by node id
        BusRef m_selected{0, -1};
        bool m_showing = false; // true while selecting from code, so the selection is not taken as the user's
        UniquePtr<foundation::gui::PropertyBag> m_busRows;    // name, parent
        UniquePtr<foundation::gui::PropertyBag> m_effectRows[4]; // the chosen bus's mix, filters, delay and reverb, one section each
        foundation::gui::TreeView* m_tree = nullptr;
        foundation::gui::ButtonBase* m_remove = nullptr;
        RefPtr<foundation::gui::Menu> m_menu;
    };

    class AudioBusLayoutPageFactory final : public IEditorPageFactory
    {
    public:
        explicit AudioBusLayoutPageFactory(const app::AssetPageServicesSource& services) : m_services(&services) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override;
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override;

    private:
        const app::AssetPageServicesSource* m_services = nullptr;
    };
}
