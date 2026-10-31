// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - the `editor.app:tool_panel` partition.
//
// Viewport tool panels. A viewport tool (editor.viewport-tools) is a UI-free modal interaction
// mode; some tools are also modes that want an on-screen settings surface while they are active,
// such as a terrain brush's panel. That surface builds GUI nodes, so it cannot live on
// IViewportTool: the tool framework stays UI-free because headless hosts link it. Instead there is
// a second registry one layer up, in the editor UI tier. UI-capable domain libraries register an
// IViewportToolPanelProvider keyed by their tool's Id(), and whenever the active tool changes the
// scene page mounts the matching panel (or nothing) in one place, the HUD's tool panel.
//
// The panel factory receives the same ViewportToolHostContext the tool did (scene, command stack,
// selection), and a panel lives only while its tool is active: the view is created on activation
// and dropped on deactivation, so durable edit state must live in the tool or its domain library,
// never in the view. That keeps every panel safe to recreate, and adding another mode only means
// registering another provider for another tool id.

module;
#include "Core/Prelude.h"

export module editor.app:tool_panel;

import foundation.core;
import foundation.gui;
import editor.viewporttools;

using namespace foundation::core;

export namespace editor
{
    /// A UI-tier settings panel for a viewport tool mode. A UI-capable domain editor library
    /// implements this and registers one per tool id; the host mounts the returned node while that
    /// tool is active. Providers have static lifetime and are registered explicitly, never
    /// discovered.
    class IViewportToolPanelProvider
    {
    public:
        virtual ~IViewportToolPanelProvider() = default;

        /// The tool this panel belongs to; matches IViewportTool::Id(). Stable, never localized.
        [[nodiscard]] virtual StringView ToolId() const = 0;

        /// Builds the panel's body for one host activation (the host gives it its chrome: the
        /// tool's name, fold and close). Receives the active tool this panel is for, which a
        /// provider downcasts to its concrete tool (the id matched, so the type is known) to reach
        /// state the UI-free IViewportTool cannot expose, plus the same context the tool got. May
        /// return null when this context has nothing to show; the host then shows no panel.
        [[nodiscard]] virtual RefPtr<foundation::gui::Node> CreatePanel(IViewportTool& tool, const ViewportToolHostContext& context) = 0;
    };

    /// Registry of tool panel providers, keyed by tool id. A process-wide singleton like the tool
    /// provider registry it parallels (ViewportToolProviderRegistry), but constructible standalone
    /// so tests can exercise a host against a private set of providers.
    class ViewportToolPanelRegistry
    {
    public:
        ViewportToolPanelRegistry() = default;

        /// The process-wide instance the scene page consumes.
        [[nodiscard]] static ViewportToolPanelRegistry& Get();

        /// Registers a provider (static lifetime, borrowed). Idempotent on the same pointer; a
        /// second provider for an already-registered tool id is ignored (first wins) so a
        /// double-registered registrar cannot silently swap a page's panel.
        void Register(IViewportToolPanelProvider* provider);

        [[nodiscard]] usize Count() const noexcept { return m_providers.Size(); }

        /// The provider for a tool id, or null when that tool has no panel.
        [[nodiscard]] IViewportToolPanelProvider* FindByToolId(StringView toolId) noexcept;

    private:
        Array<IViewportToolPanelProvider*> m_providers;
    };

    /// Watches a ViewportToolManager's active tool and mounts or unmounts the matching panel
    /// through host-supplied callbacks. It is pure control logic (no widget of its own), so it is
    /// unit-testable without a live viewport; the host owns where the panel shows. Call Sync() once
    /// per frame from the host's update, not during event dispatch, because mounting tears down the
    /// previous panel.
    class ViewportToolPanelHost
    {
    public:
        /// `mount` receives the freshly built panel and the tool it is for; `clear` is asked to
        /// remove whatever `mount` last showed. Both fire only when the active tool changes.
        ViewportToolPanelHost(ViewportToolManager& tools, ViewportToolPanelRegistry& registry, ViewportToolHostContext context,
                              Function<void(foundation::gui::Node*, IViewportTool&)> mount, Function<void()> clear);

        /// Remounts the panel if the active tool changed since the last call; otherwise does nothing.
        void Sync();

        [[nodiscard]] foundation::gui::Node* CurrentPanel() const noexcept { return m_current.Get(); }
        [[nodiscard]] StringView CurrentToolId() const noexcept { return m_currentId.AsView(); }

    private:
        ViewportToolManager* m_tools;
        ViewportToolPanelRegistry* m_registry;
        ViewportToolHostContext m_context;
        Function<void(foundation::gui::Node*, IViewportTool&)> m_mount;
        Function<void()> m_clear;
        RefPtr<foundation::gui::Node> m_current; // keeps the mounted panel alive for its activation
        String m_currentId;                      // the active tool id at the last Sync ("" = none)
    };
}
