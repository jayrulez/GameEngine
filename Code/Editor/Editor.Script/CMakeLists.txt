# Editor::Script - `editor.script`: the script page on the GUI (a behaviour's code, checked as it is
# typed, completion and hover from the API its language binds, the API browser, breakpoints) on the
# code pages' base. Editor exe only.
util_add_engine_library(Editor.Script ALIAS Editor::Script)
target_link_libraries(Editor.Script PUBLIC
    Foundation::Core Foundation::Content Foundation::Runtime.Client Foundation::GUI Foundation::GUI.Code
    Foundation::Script Foundation::Script.Facades Pipeline::Script Foundation::Script.Resource
    Editor::Core Editor::App Foundation::Policy)
target_sources(Editor.Script
    PRIVATE
        ScriptPageImpl.cpp ScriptApiSurfaceImpl.cpp ScriptApiBrowserImpl.cpp
    PUBLIC
        FILE_SET editor_script_modules TYPE CXX_MODULES FILES
            ScriptPage.cppm ScriptApi.cppm ScriptLanguageService.cppm)
