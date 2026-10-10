// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Project - `editor.project`: the headless project half of the editor domain - the
// open project, the cook service, export, jobs, the log buffer, the project registry and
// manager, the root allocator seam. What a host without an editor (the MCP stdio server,
// the cook and export tools) stands on; editor.core adds the context, pages, commands,
// selection, actions and settings on top and re-exports this.
export module editor.project;

export import :allocator;
export import :project;
export import :project_registry;
export import :project_manager;
export import :cook_service;
export import :log_buffer;
export import :job_service;
export import :export_preset;
export import :export_roots;
export import :template_icons;
export import :export_template;
export import :export_controller;
export import :export_pipeline;
