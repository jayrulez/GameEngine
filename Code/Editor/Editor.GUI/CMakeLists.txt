# Editor::GUI - `editor.gui`: the editor's pages for the engine GUI's assets on the GUI's code pages:
# a .gml document and a .css sheet, each its code with a live preview drawn by the runtime's GUI.
util_add_engine_library(Editor.GUI ALIAS Editor::GUI)
target_link_libraries(Editor.GUI PUBLIC
    Foundation::Core Foundation::Content Foundation::Runtime.Client Foundation::Settings Foundation::Graphics Foundation::RHI
    Foundation::GUI Foundation::GUI.Code Foundation::GUI.Viewport Pipeline::GUI Pipeline::Core Engine::GUI
    Editor::Core Editor::App Foundation::Policy)
target_sources(Editor.GUI
    PRIVATE GuiPagesImpl.cpp
    PUBLIC FILE_SET editor_gui_modules TYPE CXX_MODULES FILES GuiPages.cppm)
