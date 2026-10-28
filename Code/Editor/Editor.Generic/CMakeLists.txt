# Editor::Generic - `editor.generic`: the serialize-driven FALLBACK asset
# page (every asset without a bespoke page), on the GUI. Linked by the editor EXECUTABLE only.
util_add_engine_library(Editor.Generic ALIAS Editor::Generic)
target_link_libraries(Editor.Generic
    PUBLIC
        Foundation::Core Foundation::Content
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Foundation::Policy)

target_sources(Editor.Generic
    PRIVATE
        AssetFormPageImpl.cpp
    PUBLIC
        FILE_SET editorgeneric_modules TYPE CXX_MODULES FILES
            AssetFormPage.cppm)

