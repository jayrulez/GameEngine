// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - :editor_dock partition.
//
// EditorDock: the editor's dock. A GUI.Docking controller with its area in the frame's
// `frame-dock`, its floating panels in OS windows of their own where the GUI's host can open them
// (else over the main window), and the panels every project shares: the Project panel (the open
// project's page, or the welcome without one), Assets, Console and the Profiler (a tab in the page
// area, absent from the default layout until shown). Their ids are the ones saved layouts and the
// MCP layout tools use: `welcome` for the Project panel, `assets`, `console`, `profiler`.
module;
#include "Core/Prelude.h"

export module editor.app:editor_dock;

import foundation.core;
import foundation.gui;
import foundation.gui.hosting;
import foundation.gui.docking;

using namespace foundation::core;

export namespace editor::app
{
    namespace gui = foundation::gui;

    inline constexpr StringView kPanelProject = u8"welcome";
    inline constexpr StringView kPanelAssets = u8"assets";
    inline constexpr StringView kPanelConsole = u8"console";
    inline constexpr StringView kPanelProfiler = u8"profiler";

    class EditorDock
    {
    public:
        /// The factories for its panels' contents (built from the window's markup, for example).
        struct Contents
        {
            Function<RefPtr<gui::Node>()> welcome; ///< the Project panel without a project
            Function<RefPtr<gui::Node>()> assets;
            Function<RefPtr<gui::Node>()> console;
            Function<RefPtr<gui::Node>()> profiler;
        };

        explicit EditorDock(IAllocator& allocator) : m_allocator(allocator), m_controller(allocator) {}
        EditorDock(const EditorDock&) = delete;
        EditorDock& operator=(const EditorDock&) = delete;
        ~EditorDock() { Close(); }

        /// Registers the shared panels, takes the default layout and shows it in `into` (the
        /// frame's dock node). `host` (nullable) opens the floating panels' windows when it can.
        void Open(gui::Node& into, gui::GuiHost* host, Contents contents)
        {
            Close();
            m_contents = Move(contents);
            EditorDock* self = this;
            RegisterPanel(kPanelProject, u8"Project", false, [self]() { return self->Holder(); });
            RegisterPanel(kPanelAssets, u8"Assets", true, [self]() { return self->Make(self->m_contents.assets); });
            RegisterPanel(kPanelConsole, u8"Console", true, [self]() { return self->Make(self->m_contents.console); });
            RegisterPanel(kPanelProfiler, u8"Profiler", true, [self]() { return self->Make(self->m_contents.profiler); });
            AdoptDefault();
            m_area = MakeRef<gui::DockArea>(m_allocator);
            into.AddChild(m_area.Get());
            m_area->Show(m_controller);
            if (host != nullptr && host->CanOpenWindows())
            {
                m_windows = MakeUnique<gui::DockHostWindows>(m_allocator, m_allocator, *host, *m_area);
                m_area->SetWindows(m_windows.Get());
                if (m_area->GetWindows() == nullptr)
                    m_windows.Reset(); // a window failed to open: floating panels stay over the main window
            }
        }

        /// Takes down the area and its windows (before the GUI's host goes away).
        void Close()
        {
            if (m_area)
            {
                m_area->SetWindows(nullptr);
                m_windows.Reset();
                m_area->RemoveFromParent();
                m_area = {};
            }
        }

        /// The default layout: the Project panel in the central stack, under it a pane of Assets
        /// and Console side by side (the document 70% of the height; Assets 65% of the pane).
        [[nodiscard]] static gui::DockLayout DefaultLayout()
        {
            gui::DockLayout layout;
            const gui::DockNodeId central = layout.CentralStack();
            (void)layout.InsertTab(kPanelProject, central);
            (void)layout.InsertBeside(kPanelAssets, central, gui::DockSide::Bottom, kPaneShare);
            (void)layout.InsertBeside(kPanelConsole, layout.StackOf(kPanelAssets), gui::DockSide::End, kConsoleShare);
            return layout;
        }
        /// Adopts the default layout again (View > Reset Layout).
        void ResetLayout() { AdoptDefault(); }
        /// Brings a panel to the front: shows it at its default place when it is closed, else
        /// selects its tab.
        void Present(StringView panel)
        {
            if (!m_controller.Show(panel).Ok())
                (void)m_controller.Edit([panel](gui::DockLayout& layout) { return layout.Activate(panel); });
        }

        /// Sets the Project panel's content: the open project's page, or the welcome when null. The
        /// panel's own content is a holder for it, so it shows wherever the panel is docked.
        void SetProjectContent(RefPtr<gui::Node> content)
        {
            m_project = Move(content);
            if (m_holder)
                FillHolder();
        }

        [[nodiscard]] gui::DockController& Controller() noexcept { return m_controller; }
        [[nodiscard]] gui::DockArea* Area() const noexcept { return m_area.Get(); }
        /// Whether its floating panels open as OS windows (else over the main window).
        [[nodiscard]] bool FloatsInWindows() const noexcept { return m_windows.Get() != nullptr; }

    private:
        static constexpr f32 kPaneShare = 0.3f;

        // Adopts the default layout with the Profiler closed. Adopting places every registered
        // panel; the Profiler is a page, shown when asked for (View > Profiler) where it was last.
        void AdoptDefault()
        {
            (void)m_controller.Adopt(DefaultLayout());
            (void)m_controller.Close(kPanelProfiler);
        }
        static constexpr f32 kConsoleShare = 0.35f;

        void RegisterPanel(StringView id, StringView title, bool closable, Function<RefPtr<gui::Node>()> factory)
        {
            if (m_controller.Describe(id) != nullptr)
                return; // already registered when the dock was opened before
            gui::DockPanelDescription description;
            description.Id = String(id);
            description.Title = String(title);
            description.Closable = closable;
            description.Default.Central = id == kPanelProject || id == kPanelProfiler; // the Profiler is a page
            description.Default.Side = gui::DockSide::Bottom;
            description.Factory = Move(factory);
            (void)m_controller.Register(Move(description));
        }
        [[nodiscard]] RefPtr<gui::Node> Holder()
        {
            if (!m_holder)
            {
                m_holder = MakeRef<gui::Panel>(m_allocator);
                m_holder->AddClass(u8"project-panel");
                FillHolder();
            }
            return RefPtr<gui::Node>(m_holder);
        }
        void FillHolder()
        {
            RefPtr<gui::Node> shown = m_project;
            if (!shown)
            {
                if (!m_welcome)
                    m_welcome = Make(m_contents.welcome);
                shown = m_welcome;
            }
            if (m_holder->ChildCount() == 1 && m_holder->GetChildAt(0) == shown.Get())
                return;
            while (m_holder->ChildCount() > 0)
                m_holder->RemoveChild(m_holder->GetChildAt(m_holder->ChildCount() - 1));
            if (shown->GetParent() != nullptr)
                shown->RemoveFromParent();
            m_holder->AddChild(shown.Get());
        }
        [[nodiscard]] RefPtr<gui::Node> Make(const Function<RefPtr<gui::Node>()>& make)
        {
            if (make)
                if (RefPtr<gui::Node> made = make())
                    return made;
            return RefPtr<gui::Node>(MakeRef<gui::Panel>(m_allocator));
        }
        IAllocator& m_allocator;
        gui::DockController m_controller;
        RefPtr<gui::DockArea> m_area;
        UniquePtr<gui::DockHostWindows> m_windows;
        Contents m_contents;
        RefPtr<gui::Panel> m_holder; // the Project panel's content
        RefPtr<gui::Node> m_welcome;
        RefPtr<gui::Node> m_project;
    };
}
