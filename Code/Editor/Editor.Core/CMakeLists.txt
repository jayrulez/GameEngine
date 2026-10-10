# Editor::Core - `editor.core` (the editor-facing half of the editor domain: context,
# pages, command stack, selection, actions, thumbnails, per-user settings) over
# Editor::Project, which it re-exports. Tooling only - the runtime never links this.

util_add_engine_library(Editor.Core ALIAS Editor::Core)
target_link_libraries(Editor.Core
    PUBLIC Editor::Project
           Foundation::Mcp # domain-contributed MCP tools (EditorContext::RegisterMcpToolContribution)
           Foundation::Image Foundation::Image.IO Foundation::UI)

target_sources(Editor.Core
    PRIVATE
        ThumbnailServiceImpl.cpp
        ContextImpl.cpp
        CommandImpl.cpp
    PUBLIC
        FILE_SET editorcore_modules TYPE CXX_MODULES FILES
            Command.cppm
            Selection.cppm
            Page.cppm
            Actions.cppm
            Context.cppm
            SearchFilter.cppm
            ThumbnailService.cppm
            EditorSettings.cppm
            EditorCoreModule.cppm)
