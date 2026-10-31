// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Scene - :registration partition.
//
// RegisterSceneEditor, the module's entry point the executable calls (the editor's core and app
// never link this module). It registers the scene editor's services (the document types, the
// export's extension points, a model import's prefab), its thumbnail generators, a page factory for
// scenes and one for prefabs (a prefab's payload is a scene stream, so the same page edits it), the
// scene editor's actions and its MCP live tools. A page gets from the editor what draws its
// viewport, its markup and the asset picker, asked for when it is made (the window's GUI host
// exists once the window is open).

module;
#include "Core/Prelude.h"

export module editor.scene:registration;

import foundation.core;
import foundation.content;
import foundation.runtime.client;
import foundation.scene;
import foundation.mcp;
import editor.core;
import editor.app;
import :page;
import :services;
import :asset_thumbnails;
import :actions;
import :mcp_tools;
import :inspector_extras;
import :builtin_inspector_extras;

using namespace foundation::core;

export namespace editor
{
    class SceneEditorPageFactory final : public IEditorPageFactory
    {
    public:
        SceneEditorPageFactory(const TypeInfo& type, Function<SceneEditorPage::Services()> services) : m_type(&type), m_services(Move(services)) {}
        [[nodiscard]] const TypeInfo* PrimaryType() const override { return m_type; }
        [[nodiscard]] UniquePtr<EditorPage> CreatePage(EditorContext& context, foundation::content::Instance& instance) override
        {
            return UniquePtr<EditorPage>(EditorRootAllocator().New<SceneEditorPage>(context, m_services(), instance), EditorRootAllocator());
        }

    private:
        const TypeInfo* m_type;
        Function<SceneEditorPage::Services()> m_services;
    };

    inline void RegisterSceneEditor(EditorContext& context, foundation::runtime::IApplicationHost& host, app::EditorApplication& app)
    {
        RegisterSceneEditorServices(context, host);
        // The pictures of meshes, materials, prefabs, scenes and effects: the generators queue them
        // and the application's thumbnail stage renders them.
        if (context.Thumbnails() != nullptr)
            RegisterSceneThumbnailGenerators(*context.Thumbnails(), context);

        foundation::runtime::IApplicationHost* appHost = &host;
        app::EditorApplication* editor = &app;
        const auto services = [appHost, editor]()
        {
            SceneEditorPage::Services pageServices;
            pageServices.host = appHost;
            pageServices.gui = editor->Window().Host();
            app::EditorWindow* window = &editor->Window();
            pageServices.loadMarkup = [window](StringView path) { return window->LoadMarkup(path); };
            pageServices.refs = &editor->RefServices();
            pageServices.icons = &editor->TypeIcons();
            pageServices.pick = [editor](app::PickerRequest request) { editor->ShowPicker(Move(request)); };
            pageServices.createAsset = [editor](StringView title, StringView suggested, foundation::content::Group* group,
                                                Function<void(foundation::content::Group&, StringView)> create)
            { editor->ShowCreateAsset(title, suggested, group, Move(create)); };
            return pageServices;
        };
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            EditorRootAllocator().New<SceneEditorPageFactory>(foundation::scene::SceneDocument::StaticType(), services), EditorRootAllocator()));
        context.Pages().Register(UniquePtr<IEditorPageFactory>(
            EditorRootAllocator().New<SceneEditorPageFactory>(foundation::scene::PrefabDocument::StaticType(), services), EditorRootAllocator()));

        // What the inspector shows under the engine's own components besides their rows.
        static const bool extras = []()
        {
            BuiltinInspectorExtras::Register(InspectorExtrasRegistry::Get());
            return true;
        }();
        (void)extras;
        // The scene editor's actions: the Scene menu, the keys and the page's bar are served from
        // them; the palette and the MCP bridge read them.
        RegisterSceneEditorActions(context);
        // Its MCP tools, served by the editor's host over whichever scene page a call addresses.
        EditorContext* ctx = &context;
        context.RegisterMcpToolContribution([ctx](foundation::mcp::McpServer& server) { RegisterSceneLiveTools(server, *ctx); });
    }
}
