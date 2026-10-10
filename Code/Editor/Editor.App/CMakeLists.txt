# Editor::App - `editor.app`: the editor UI shell (chrome + layout persistence +
# EditorApplication) on foundation.ui / toolkit / the runtime docking host. Tooling only.

util_add_engine_library(Editor.App ALIAS Editor::App)
target_link_libraries(Editor.App
    PUBLIC
        Foundation::Core Foundation::VFS Foundation::Xml.Serialization Pipeline::Core Foundation::Resource
        Foundation::Shell Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client Engine::DefaultApp
        Foundation::Render.Api
        Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Resource Foundation::Fonts.DistanceField.Baker
        Foundation::UI Foundation::UI.Toolkit Foundation::UI.Runtime Foundation::UI.Application
        Foundation::UI.Resource Engine::UI
        Foundation::Input Engine::Input Editor::Preview
        Editor::Core Editor::ViewportTools Foundation::Settings Foundation::Policy
        Editor::Mcp Foundation::Mcp Foundation::Mcp.Http) # the MCP host: the shared engine surface over HTTP

target_sources(Editor.App
    PRIVATE
        ApplicationImpl.cpp
        AssetsViewImpl.cpp
        AssetPickerDialogImpl.cpp
        SettingsDialogImpl.cpp
        EditorIconsImpl.cpp
        ToolPanelImpl.cpp
        ContainerListEditorImpl.cpp
        ResourceRefEditorImpl.cpp
        CompactAssetSlotImpl.cpp
        ListHeaderImpl.cpp
        ViewModeTogglesImpl.cpp
        EditorWindowToolsImpl.cpp
        EditorMcpHostImpl.cpp
        EditorProjectOperationsImpl.cpp
        EditorPageToolsImpl.cpp EditorActionToolsImpl.cpp
    PUBLIC
        FILE_SET editorapp_modules TYPE CXX_MODULES FILES
            Layout.cppm
            LogView.cppm
            UIPage.cppm
            PageToolbar.cppm
            PlaybackActions.cppm
            ActionMenus.cppm
            ActionShortcuts.cppm
            ToolPanel.cppm
            ToolPanelWidgets.cppm
            AssetsView.cppm
            AssetPickerDialog.cppm
            AssetCreateDialog.cppm
            ConfirmDialog.cppm
            PathPickerDialog.cppm
            GroupPickerDialog.cppm
            ImportDialog.cppm
            SettingsDialog.cppm
            PreferencesDialog.cppm
            CommandPalette.cppm
            ShortcutCapture.cppm
            EditorIcons.cppm
            AssetPickerSlot.cppm
            ContainerListEditor.cppm
            ResourceRefEditor.cppm
            CompactAssetSlot.cppm
            ListHeader.cppm
            ViewModeToggles.cppm
            AssetFilter.cppm
            CategoryTabs.cppm
            WindowCapture.cppm
            EditorWindowTools.cppm
            EditorMcpHost.cppm
            EditorProjectOperations.cppm
            EditorPageTools.cppm
            EditorActionTools.cppm
            AssetDragData.cppm
            ProjectManagerView.cppm
            Shell.cppm
            FontAtlasDiskCache.cppm
            Application.cppm
            EditorAppModule.cppm)

