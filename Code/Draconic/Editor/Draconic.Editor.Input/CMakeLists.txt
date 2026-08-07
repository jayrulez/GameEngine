# Draconic::Editor.Input - `draconic.editor.input`: the InputMapAsset editing page + factory.
# Linked by the editor EXECUTABLE only.
add_library(Draconic.Editor.Input STATIC)
add_library(Draconic::Editor.Input ALIAS Draconic.Editor.Input)

target_link_libraries(Draconic.Editor.Input
    PUBLIC
        Draconic::Foundation Draconic::Content Draconic::Shell
        Draconic::Runtime Draconic::Runtime.Client
        Draconic::Input Draconic::Input.Resource Draconic::Input.Editor
        Draconic::UI Draconic::UI.Runtime Draconic::UI.Toolkit
        Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)

target_sources(Draconic.Editor.Input
    PRIVATE
        InputMapPageImpl.cpp
    PUBLIC
        FILE_SET editorinput_modules TYPE CXX_MODULES FILES
            InputMapPage.cppm)
