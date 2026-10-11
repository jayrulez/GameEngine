// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :shell partition.
//
// EditorShell: the editor chrome - a RootView holding
// [MenuBar / DockManager (grow) / StatusBar]. GLOBAL panels are Assets + Console only:
// everything scene-scoped (viewport, hierarchy, inspector, selection,
// camera) lives INSIDE each editor page, because multi-scene editing means several scene
// pages can be open at once - per-page views, never global panels. The dock CENTER is
// the document area: each open page docks there as a closable tab via AddPagePanel; a
// non-closable Welcome panel holds the center until the first page opens (and keeps the
// center tab group alive when all pages close).

module;
#include "Core/Prelude.h"

export module editor.app:shell;

import foundation.core;
import foundation.ui;
import foundation.settings;
import foundation.ui.toolkit;
import editor.core;
import :layout;
import :log_view;

using namespace foundation::core;

export namespace editor::app
{
    namespace ui = foundation::ui;

    // Stable persistence ids for the GLOBAL panels (LoadDockLayout match keys - do not rename).
    inline constexpr StringView kPanelWelcome = u8"welcome";
    inline constexpr StringView kPanelConsole = u8"console";
    inline constexpr StringView kPanelAssets = u8"assets";

    class EditorShell
    {
    public:
        EditorShell() = default;
        EditorShell(const EditorShell&) = delete;
        EditorShell& operator=(const EditorShell&) = delete;

        // Build the chrome. `dockHost` (nullable) lets panels float into real OS windows.
        /// `logo` (LoadEditorLogo) heads the Welcome page; null shows the name as text.
        void Build(editor::EditorContext& context,
                   ui::toolkit::IDockableWindowHost* dockHost, u32 width, u32 height,
                   ui::DrawablePtr logo = {})
        {
            m_logo = Move(logo);
            m_root = MakeRef<ui::RootView>(editor::EditorRootAllocator());
            m_root->ViewportSize = Float2{static_cast<f32>(width), static_cast<f32>(height)};
            m_root->DpiScale = 1.0f;

            auto column = MakeRef<ui::FlexLayout>(editor::EditorRootAllocator());
            column->Direction = ui::Orientation::Vertical;

            m_menuBar = MakeRef<ui::toolkit::MenuBar>(editor::EditorRootAllocator());
            column->AddView(m_menuBar.Get());

            m_dock = MakeRef<ui::toolkit::DockManager>(editor::EditorRootAllocator());
            m_dock->DockableWindowHost = dockHost;
            {
                ui::LayoutStyle grow;
                grow.FlexGrow = 1.0f;
                column->AddView(m_dock.Get(), grow);
            }

            m_statusBar = MakeRef<ui::toolkit::StatusBar>(editor::EditorRootAllocator());
            column->AddView(m_statusBar.Get());

            m_root->AddView(column.Get());

            BuildPanels();

            // Route context status text to the status bar.
            ui::toolkit::StatusBar* status = m_statusBar.Get();
            context.OnStatus = [status](StringView text) { status->SetText(text); };
        }

        [[nodiscard]] ui::RootView* Root() const noexcept { return m_root.Get(); }
        [[nodiscard]] ui::toolkit::MenuBar* Menus() const noexcept { return m_menuBar.Get(); }
        [[nodiscard]] ui::toolkit::StatusBar* StatusBar() const noexcept
        {
            return m_statusBar.Get();
        }
        [[nodiscard]] ui::toolkit::DockManager* Docks() const noexcept { return m_dock.Get(); }

        [[nodiscard]] ui::toolkit::DockablePanel* WelcomePanel() const noexcept
        {
            return m_welcome;
        }
        [[nodiscard]] ui::toolkit::DockablePanel* ConsolePanel() const noexcept
        {
            return m_console;
        }
        [[nodiscard]] ui::toolkit::DockablePanel* AssetsPanel() const noexcept { return m_assets; }

        /// Replace the Assets panel's placeholder with the real browser (once the project +
        /// cook service exist).
        void SetAssetsContent(foundation::ui::View* content)
        {
            if (m_assets != nullptr)
            {
                m_assets->SetContent(content);
            }
        }

        /// The Console panel's log view (fed by the app's EditorLogBuffer drain).
        [[nodiscard]] LogView* Console() const noexcept { return m_logView.Get(); }

        // === Document area ===

        /// Dock a page's content view as a closable tab in the center document area (tabbed
        /// with the other open pages). The returned panel is owned by the DockManager;
        /// `onCloseRequested` is where the app routes dirty-check + EditorContext::ClosePage.
        ui::toolkit::DockablePanel* AddPagePanel(StringView title, ui::View* content)
        {
            ui::toolkit::DockablePanel* panel = m_dock->AddPanel(title, content);
            panel->SetClosable(true);
            m_dock->DockPanelRelativeTo(panel, ui::toolkit::DockPosition::Center,
                                        m_welcome->Parent);
            return panel;
        }

        // Dock-layout persistence via the per-project settings STORE (the app owns loading
        // and saving the store's file); these only capture/apply the dock snapshot section.
        [[nodiscard]] Status SaveLayout(foundation::settings::Settings& store) const
        {
            return CaptureDockLayout(*m_dock, store);
        }
        [[nodiscard]] Status RestoreLayout(foundation::settings::Settings& store) const
        {
            return ApplyDockLayout(*m_dock, store);
        }

        /// Rebuild the default arrangement (View > Reset Layout).
        void ResetLayout() { DockDefaults(); }

    private:
        // The Welcome page: the logo (else the name) over a quiet line, centred in the panel.
        RefPtr<ui::FlexLayout> BuildWelcome()
        {
            IAllocator& allocator = editor::EditorRootAllocator();
            auto page = MakeRef<ui::FlexLayout>(allocator);
            page->Direction = ui::Orientation::Vertical;
            page->JustifyContent = ui::Justify::Center;
            page->AlignItems = ui::Align::Center;
            page->Spacing = 14;
            if (m_logo.Get() != nullptr)
            {
                constexpr f32 kLogoHeight = 72.0f;
                auto logo = MakeRef<ui::DrawableView>(allocator, m_logo, kLogoHeight * 931.0f / 200.0f, kLogoHeight);
                logo->Opacity = 0.9f;
                page->AddView(logo.Get());
            }
            else
            {
                auto name = MakeRef<ui::Label>(allocator, StringView(u8"AssiduousEngine"));
                name->FontSize.SetValue(28.0f);
                page->AddView(name.Get());
            }
            auto hint = MakeRef<ui::Label>(allocator, StringView(u8"Open an asset to begin"));
            hint->FontSize.SetValue(13.0f);
            hint->TextColor.SetValue(Optional<Color>(Color{0.55f, 0.55f, 0.57f, 1.0f}));
            page->AddView(hint.Get());
            return page;
        }

        void BuildPanels()
        {
            m_welcome = m_dock->AddPanel(u8"Welcome", BuildWelcome().Get());
            m_welcome->SetPersistenceId(kPanelWelcome);
            m_welcome->SetClosable(false);

            m_logView = MakeRef<LogView>(editor::EditorRootAllocator());
            m_console = m_dock->AddPanel(u8"Console", m_logView.Get());
            m_console->SetPersistenceId(kPanelConsole);

            m_assets = m_dock->AddPanel(
                u8"Assets",
                MakeRef<ui::Label>(editor::EditorRootAllocator(), StringView(u8"Asset browser (phase 6)"))
                    .Get());
            m_assets->SetPersistenceId(kPanelAssets);

            DockDefaults();
        }

        void DockDefaults()
        {
            // Documents center, the south tool pane below (Sedulous's layout; scene-scoped
            // views live inside the pages). The pane is Assets on the left and Console on the
            // right, side by side so both show at once, 65/35; the pane takes 30% of the
            // height, the document 70%. (A dock inserts its split at 0.5; the ratio is the
            // first child's share, so the root's is the document's and the pane's is Assets'.)
            m_dock->DockPanel(m_welcome, ui::toolkit::DockPosition::Center);
            m_dock->DockPanel(m_assets, ui::toolkit::DockPosition::Bottom);
            m_dock->DockPanelRelativeTo(m_console, ui::toolkit::DockPosition::Right,
                                        m_assets->Parent);
            if (auto* split = Cast<ui::toolkit::DockSplit>(m_dock->RootNode()))
            {
                split->SetSplitRatio(kDefaultDocumentShare);
            }
            if (auto* pane = Cast<ui::toolkit::DockSplit>(
                    m_assets->Parent != nullptr ? m_assets->Parent->Parent : nullptr))
            {
                pane->SetSplitRatio(kDefaultAssetsShare);
            }
        }

        /// The document area's share of the main window height in the default layout.
        static constexpr f32 kDefaultDocumentShare = 0.7f;
        /// The asset browser's share of the bottom pane's width; the console has the rest.
        static constexpr f32 kDefaultAssetsShare = 0.65f;

        RefPtr<ui::RootView> m_root;
        RefPtr<ui::toolkit::MenuBar> m_menuBar;
        RefPtr<ui::toolkit::StatusBar> m_statusBar;
        RefPtr<ui::toolkit::DockManager> m_dock;
        RefPtr<LogView> m_logView;

        // Borrowed - the DockManager owns registered panels.
        ui::toolkit::DockablePanel* m_welcome = nullptr;
        ui::DrawablePtr m_logo; // the Welcome page's heading (null: the name as text)
        ui::toolkit::DockablePanel* m_console = nullptr;
        ui::toolkit::DockablePanel* m_assets = nullptr;
    };
}
