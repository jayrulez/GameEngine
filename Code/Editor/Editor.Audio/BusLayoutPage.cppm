// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Audio - the `:bus_layout_page` partition.
//
// AudioBusLayoutEditorPage: the mixer's buses, on the GUI from its markup (Data/Editor/
// BusLayoutPage.xml; Documentation/Specs/editor-gui.md, 5.4, layer 5). The buses' tree (the four
// fixed ones, Master and Effects, Music and UI under it, and the custom ones under their parents)
// on the GUI's tree beside the grid, which shows the chosen bus: a custom one's name and parent (only
// the parents that make no cycle offered), then its mix (volume, mute), its filters, its delay and
// its reverb (each off at 0). The tree's bar and a row's menu add a bus and remove a custom one.
// Each edit is one undo step; the rules they keep (a name a bus's own, children following a rename,
// moving to Master when their parent goes) are the :bus_layout_model's. A layout whose buses make a
// cycle (one the cook refuses) says so.

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
        /// A bus under Master in the first free slot, chosen; false when every slot is in use.
        bool AddBus();
        /// The chosen bus taken out (a custom one; its children move to Master), Master chosen.
        bool RemoveSelected();

        // === For tests ===
        /// The tree's rows, indented by depth ("Master\n  Effects\n    Footsteps").
        [[nodiscard]] String Tree() const;
        [[nodiscard]] foundation::gui::PropertyBag& BusRows() noexcept { return *m_busRows; }
        /// The chosen bus's settings of one group: "Mix", "Filters", "Delay" or "Reverb".
        [[nodiscard]] foundation::gui::PropertyBag& EffectRows(StringView group);
        /// An edit of a row as the grid commits one (`rows` one of the page's bags).
        void EditRow(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);

    protected:
        void OnObjectChanged() override;

    private:
        [[nodiscard]] pipeline::AudioBusLayoutAsset* Layout() const { return ObjectAs<pipeline::AudioBusLayoutAsset>(); }
        void BuildRows();
        /// The tree from the layout, the chosen bus's row selected.
        void ShowTree();
        /// The chosen bus's rows: its name and parent shown for a custom one, its values.
        void ShowBus();
        void Committed(foundation::gui::PropertyBag& rows, StringView path, const foundation::gui::Variant& value);
        void ShowMenu(i32 item, Float2 at);

        UniquePtr<foundation::gui::TreeModel> m_model;
        Array<BusRow> m_rows; // the tree's, by node id
        BusRef m_selected{0, -1};
        bool m_showing = false; // selecting from code: not the user's choice
        UniquePtr<foundation::gui::PropertyBag> m_busRows;    // name, parent
        UniquePtr<foundation::gui::PropertyBag> m_effectRows[4]; // its mix, filters, delay and reverb, a section each
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
