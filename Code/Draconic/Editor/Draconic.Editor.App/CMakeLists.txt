# Draconic::Editor.App - `draconic.editor.app`: the editor UI shell (chrome + layout persistence +
# EditorApplication) on draconic.ui / toolkit / the runtime docking host. Tooling only.

add_library(Draconic.Editor.App STATIC)
add_library(Draconic::Editor.App ALIAS Draconic.Editor.App)

target_link_libraries(Draconic.Editor.App
    PUBLIC
        Draconic::Foundation Draconic::VFS Draconic::Xml.Serialization Draconic::Editor Draconic::Resource
        Draconic::Shell Draconic::Graphics Draconic::Runtime Draconic::Runtime.Client Draconic::Engine.DefaultApp
        Draconic::Render.Api
        Draconic::Fonts Draconic::Fonts.TTF Draconic::Fonts.Resource Draconic::Fonts.DistanceField.Baker
        Draconic::UI Draconic::UI.Toolkit Draconic::UI.Runtime Draconic::UI.Application
        Draconic::UI.Resource Draconic::Engine.UI
        Draconic::Input Draconic::Engine.Input
        Draconic::Editor.Core Draconic::Settings Draconic::Policy)

target_sources(Draconic.Editor.App
    PRIVATE
        ApplicationImpl.cpp
        AssetsViewImpl.cpp
        AssetPickerDialogImpl.cpp
        SettingsDialogImpl.cpp
        EditorIconsImpl.cpp
    PUBLIC
        FILE_SET editorapp_modules TYPE CXX_MODULES FILES
            Layout.cppm
            LogView.cppm
            UIPage.cppm
            AssetsView.cppm
            AssetPickerDialog.cppm
            PathPickerDialog.cppm
            ImportDialog.cppm
            SettingsDialog.cppm
            PreferencesDialog.cppm
            EditorIcons.cppm
            AssetPickerSlot.cppm
            ProjectManagerView.cppm
            Shell.cppm
            Application.cppm
            EditorAppModule.cppm)

