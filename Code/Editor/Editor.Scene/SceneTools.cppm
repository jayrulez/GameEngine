// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :scene_tools partition.
//
// The scene page's viewport tools. Each is an action, `scene.tool.<id>`, that toggles it over the
// page (so the menu bar, the palette, a chord the user binds and MCP can reach it), disabled where
// it has nothing to work on and saying why. The bar shows them from the tool registry: a category
// of several tools as one menu with its first tool's icon, a lone tool as a toggle. The active
// tool's panel has one place: the HUD's top start under the chips, in a frame with the tool's name,
// a fold and a close (Esc, which returns to Select). It can be dragged by its head anywhere in the
// viewport (kept inside it as it shrinks), its place is kept per scene, and a double click on its
// head puts it back under the chips.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module editor.scene:scene_tools;

import foundation.core;
import foundation.gui;
import foundation.gui.toolkit;
import editor.core;
import editor.app;
import editor.viewporttools;

using namespace foundation::core;

export namespace editor
{
    /// The tool panel's head: what a drag holds and a double click resets.
    class ToolPanelHead final : public foundation::gui::UIWidget
    {
        RTTI_OBJECT(ToolPanelHead, foundation::gui::UIWidget)
    public:
        ToolPanelHead() { AddClass(u8"hud-panel-head"); }
        /// A drag by the head; OnDrag's `delta` is measured from the press, in pixels.
        Function<void()> OnDragStart;
        Function<void(Float2 delta)> OnDrag;
        Function<void()> OnDragEnd;
        Function<void()> OnReset;

    protected:
        void OnMouseDown(const foundation::gui::MouseEvent& event) override;
        void OnMouseMove(const foundation::gui::MouseEvent& event) override;
        void OnMouseUp(const foundation::gui::MouseEvent& event) override;

    private:
        bool m_dragging = false;
        Float2 m_from{};
    };

    class SceneTools
    {
    public:
        struct Services
        {
            EditorContext* context = nullptr;
            EditorPage* page = nullptr;
            ViewportToolManager* tools = nullptr;
            ViewportToolHostContext hostContext;
            /// The bar's buttons go before this node, in its parent (the bar).
            foundation::gui::Node* barEnd = nullptr;
            /// The viewport's pane (the panel's room, a stack) and the chips' zone it sits under
            /// until moved.
            foundation::gui::Node* pane = nullptr;
            foundation::gui::Node* zone = nullptr;
            /// Called when the panel was moved or put back, so the page keeps its view state.
            Function<void()> placeChanged;
        };

        SceneTools(IAllocator& allocator, Services services);
        ~SceneTools();
        SceneTools(const SceneTools&) = delete;
        SceneTools& operator=(const SceneTools&) = delete;

        /// The action that toggles tool `id` over a scene page (registered by the first page that
        /// has the tool; every page's would be the same).
        [[nodiscard]] static String ActionOf(StringView id) { return Format(u8"scene.tool.{}", id); }

        /// Once a frame: syncs the buttons with their actions and the panel with the active tool.
        void Sync();

        /// Where the panel was moved to (its top start corner in the viewport); null while it sits
        /// under the chips.
        [[nodiscard]] const Float2* Place() const noexcept { return m_moved ? &m_place : nullptr; }
        /// Moves the panel to `place` (kept inside the viewport), or back under the chips (null).
        void SetPlace(const Float2* place);
        void SetFolded(bool folded);

        // === For tests ===
        [[nodiscard]] foundation::gui::Node* Panel() const noexcept { return m_panelShown ? m_chrome.Get() : nullptr; }
        [[nodiscard]] StringView PanelTitle() const;
        [[nodiscard]] ToolPanelHead* Head() const noexcept { return m_head.Get(); }
        [[nodiscard]] bool Folded() const noexcept { return m_folded; }
        [[nodiscard]] usize ButtonCount() const noexcept { return m_buttons.Size(); }
        [[nodiscard]] foundation::gui::ToolMenuButton* CategoryButton(StringView category) const;

    private:
        struct Category
        {
            String name;
            foundation::gui::ToolMenuButton* button = nullptr;
            Array<String> ids;
            Array<foundation::gui::MenuItem*> items;
        };
        void RegisterActions();
        void BuildBar();
        void BuildChrome();
        void Mount(foundation::gui::Node* panel, IViewportTool& tool);
        void Clear();
        void Reparent();
        void Clamp();

        IAllocator& m_allocator;
        Services m_services;
        UniquePtr<app::PageActionBar> m_bar; // the lone tools' toggles
        Array<RefPtr<foundation::gui::ButtonBase>> m_buttons;
        Array<Category> m_categories;
        UniquePtr<ViewportToolPanelHost> m_host;
        RefPtr<foundation::gui::Panel> m_chrome; // the panel's frame: head, then the tool's body
        RefPtr<ToolPanelHead> m_head;
        foundation::gui::Image* m_icon = nullptr;
        foundation::gui::Label* m_title = nullptr;
        foundation::gui::Button* m_fold = nullptr;
        RefPtr<foundation::gui::Node> m_body; // the active tool's panel body
        bool m_panelShown = false;
        bool m_moved = false;
        bool m_folded = false;
        Float2 m_place{};
        Float2 m_dragFrom{};
    };

    RTTI_DEFINE_OBJECT(ToolPanelHead, "rtti::editor::editor")
}
