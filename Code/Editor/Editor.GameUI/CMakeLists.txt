# Editor::GameUI - `editor.gameui`: UI document page (text + live preview).
util_add_engine_library(Editor.GameUI ALIAS Editor::GameUI)
target_link_libraries(Editor.GameUI PUBLIC
    Foundation::Core Foundation::Content Foundation::Runtime.Client Foundation::UI Foundation::UI.Toolkit Foundation::Xml
    Foundation::UI.Resource Foundation::VG.Resource Pipeline::UI Engine::UI Foundation::UI.Runtime Foundation::UI.Viewport
    Foundation::Graphics Foundation::RHI Foundation::VG.Renderer
    Editor::Core Editor::App Foundation::Policy)
target_sources(Editor.GameUI
    PRIVATE UIDocumentPageImpl.cpp UIThemePageImpl.cpp
    PUBLIC FILE_SET editor_gameui_modules TYPE CXX_MODULES FILES UIThemePage.cppm UIDocumentPage.cppm)
