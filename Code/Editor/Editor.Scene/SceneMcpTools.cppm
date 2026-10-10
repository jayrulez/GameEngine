// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Scene - :mcp_tools partition.
//
// The scene editor's MCP tools - what only a live scene page can serve, registered as the
// scene editor's contribution to the editor's MCP host (EditorContext::RegisterMcpToolContribution
// from RegisterSceneEditor): selection_get / selection_set over a page's entity selection, and
// simulate_start / simulate_stop over its edit-mode Simulate, entity_inspect over an entity's
// reflected components and component_set writing one of them through the page's undo path
// (one locked, labelled step per call). Every tool is PAGE-ADDRESSED
// (`page` = the scene or prefab asset's guid, as page_list reports it), defaulting to the active
// page when that is a scene page, and every result names the page it acted on - several scene
// pages may be open, each with its own selection. The page is reached through the interface it
// publishes (EditorPage::Service<ISceneEditorPage>), and the selection through its edit context,
// so the hierarchy, the inspector and the gizmos follow an agent's selection exactly as a click.
module;
#include "Core/Prelude.h"

export module editor.scene:mcp_tools;

import foundation.core;
import foundation.json;
import foundation.mcp;
import editor.core;
import :scene_page_interface; // ISceneEditorPage (AddressedPage)

using namespace foundation::core;

export namespace editor
{
    /// The number of tools RegisterSceneLiveTools registers (selection_get / selection_set /
    /// simulate_start / simulate_stop / navigation_bake / entity_inspect / component_set /
    /// viewport_camera_get / viewport_camera_set / viewport_frame / viewport_screenshot); a
    /// tripwire like kEngineToolCount.
    /// The live editing tools (SceneMcpEditToolsImpl.cpp): entity_create, entity_update,
    /// entity_delete, component_add, component_remove, prefab_spawn, behavior_add, behavior_set.
    inline constexpr usize kSceneEditToolCount = 8;
    inline constexpr usize kSceneLiveToolCount = 11 + kSceneEditToolCount;

    void RegisterSceneLiveTools(foundation::mcp::McpServer& server, EditorContext& context);
}

namespace editor
{
    // Inside the module: what the live tools and the edit tools (SceneMcpEditToolsImpl.cpp)
    // share.

    /// The scene page a call addresses: `page` (a guid) when given, else the active page.
    struct AddressedPage
    {
        EditorPage* page = nullptr;
        ISceneEditorPage* scene = nullptr;
    };
    /// The page, or the reason it is not a scene page, for the agent to read.
    [[nodiscard]] Result<AddressedPage, String> ResolveScenePage(EditorContext& context,
                                                                 const foundation::json::JsonValue& args);
    [[nodiscard]] foundation::json::JsonValue PageJson(const EditorPage& page);
    /// [x, y, z] as a Float3; false on any other shape.
    [[nodiscard]] bool ReadFloat3(const foundation::json::JsonValue& value, Float3& out);
    /// The entity the `key` argument names in the page's scene: a guid, a name or a slash path;
    /// the reason when it names none.
    [[nodiscard]] Result<Guid, String> ResolveSceneEntity(const AddressedPage& addressed,
                                                          StringView text);
    /// The editing tools, registered by RegisterSceneLiveTools.
    void RegisterSceneEditTools(foundation::mcp::McpServer& server, EditorContext& context);
}
