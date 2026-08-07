# Draconic::Editor.GameUI - `draconic.editor.gameui`: UI document page (text + live preview).
add_library(Draconic.Editor.GameUI STATIC)
add_library(Draconic::Editor.GameUI ALIAS Draconic.Editor.GameUI)
target_link_libraries(Draconic.Editor.GameUI PUBLIC
    Draconic::Foundation Draconic::Content Draconic::Runtime.Client Draconic::UI Draconic::UI.Toolkit Draconic::Xml
    Draconic::UI.Resource Draconic::UI.Editor Draconic::Engine.UI Draconic::UI.Runtime Draconic::UI.Viewport
    Draconic::Graphics Draconic::RHI Draconic::VG.Renderer
    Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)
target_sources(Draconic.Editor.GameUI PRIVATE UIDocumentPageImpl.cpp PUBLIC FILE_SET editor_gameui_modules TYPE CXX_MODULES FILES UIDocumentPage.cppm)
