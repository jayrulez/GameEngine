# Editor::Fonts - `editor.fonts` (the FontAsset authoring page:
# live atlas bake preview + bake-intent property grid), on the GUI. Tooling only.

util_add_engine_library(Editor.Fonts ALIAS Editor::Fonts)
target_link_libraries(Editor.Fonts
    PUBLIC Foundation::Core Foundation::Content Foundation::VFS Foundation::Image
           Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.Coverage.Baker
           Foundation::Fonts.DistanceField.Baker Pipeline::Fonts
           Foundation::GUI Foundation::GUI.Toolkit Editor::Core Editor::App
           Foundation::Policy)

target_sources(Editor.Fonts
    PRIVATE
        FontEditorPageImpl.cpp
    PUBLIC
        FILE_SET editor_fonts_modules TYPE CXX_MODULES FILES
            FontEditorPage.cppm FontThumbnailGenerator.cppm)
