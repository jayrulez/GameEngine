# Editor::App - `editor.app`: the editor's shell (the window, the frame, menus and shortcuts from the
# actions, the dock and its layout, the MCP host, the project home), moving onto the GUI in parts
# (Documentation/Specs/editor-gui.md, 5.1). A file not ported yet stays out of these lists, its
# source unedited, and comes back with its port. Tooling only.

util_add_engine_library(Editor.App ALIAS Editor::App)
target_link_libraries(Editor.App
    PUBLIC
        Foundation::Core Foundation::VFS Foundation::Xml.Serialization Pipeline::Core Foundation::Resource
        Foundation::Shell Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
        Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Resource Foundation::Fonts.DistanceField.Baker
        Foundation::Fonts.HarfBuzz Foundation::Fonts.Bidi # the GUI's shaping and reordering
        Foundation::Image Foundation::Image.IO Foundation::RHI.Capture
        Foundation::Profiler Foundation::Profiler.Capture
        Foundation::VFS Foundation::GUI Foundation::GUI.Hosting Foundation::GUI.Docking Foundation::GUI.Toolkit Foundation::GUI.Code
        Foundation::Scene Foundation::Scene.Resource Engine::Scene Engine::Project # the native module's scenes carried across a reload
        Foundation::Shaders.System Engine::DefaultApp Engine::UI Engine::GUI Foundation::UI.Resource # the game UI's defaults (the runtime's)
        Editor::Core Foundation::Settings
        Editor::Mcp Foundation::Mcp Foundation::Mcp.Http) # the MCP host: the shared engine surface over HTTP

target_sources(Editor.App
    PRIVATE
        ApplicationImpl.cpp
        EditorSplashImpl.cpp
        EditorWindowToolsImpl.cpp
        EditorProfilerToolsImpl.cpp
        EditorMcpHostImpl.cpp
        EditorProjectOperationsImpl.cpp
        EditorPageToolsImpl.cpp
    PUBLIC
        FILE_SET editorapp_modules TYPE CXX_MODULES FILES
            UIPage.cppm
            EditorStatusBar.cppm
            EditorWindow.cppm
            EditorDock.cppm
            EditorPages.cppm
            ActionMenus.cppm
            ActionShortcuts.cppm
            CommandPalette.cppm
            PreferencesDialog.cppm
            EditorNativeModule.cppm
            EditorExport.cppm
            EditorAssetCreation.cppm
            Layout.cppm
            EditorSession.cppm
            EditorMcpService.cppm
            EditorAppActions.cppm
            Application.cppm
            PlaybackActions.cppm
            AssetFilter.cppm
            PropertyNames.cppm
            EditorSplash.cppm
            WindowCapture.cppm
            EditorWindowTools.cppm
            EditorProfilerTools.cppm
            EditorMcpHost.cppm
            EditorProjectOperations.cppm
            EditorPageTools.cppm
            FontAtlasDiskCache.cppm
            EditorAppModule.cppm)
