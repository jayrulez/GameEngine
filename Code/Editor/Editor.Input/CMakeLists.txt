# Editor::Input - `editor.input`: the input map page on the GUI (the sets and their actions, each
# binding's input captured by pressing it). Linked by the editor EXECUTABLE only.
util_add_engine_library(Editor.Input ALIAS Editor::Input)
target_link_libraries(Editor.Input
    PUBLIC
        Foundation::Core Foundation::Content Foundation::Shell
        Foundation::Runtime Foundation::Runtime.Client
        Foundation::Input Foundation::Input.Resource Pipeline::Input
        Foundation::GUI Foundation::GUI.Toolkit
        Editor::Core Editor::App Foundation::Policy)

target_sources(Editor.Input
    PRIVATE
        InputMapPageImpl.cpp
    PUBLIC
        FILE_SET editorinput_modules TYPE CXX_MODULES FILES
            InputMapPage.cppm InputMapEdits.cppm InputMapRows.cppm InputCaptureEditor.cppm)
