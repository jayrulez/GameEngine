# Experimental::GUI.Inspector - `experimental.gui.inspector`, the GUI lab's inspector (E8e of
# Specs/gui-lab-evolution.md): queries over a GUI's tree (the tree, a node described, a selector
# query) and the panes built on them from the lab's own widgets. A library over the core's
# public API, so a host shows it beside what it inspects and a tool can ask the same questions.
util_add_engine_library(GUI.Inspector ALIAS Experimental::GUI.Inspector)
target_link_libraries(GUI.Inspector
    PUBLIC Experimental::GUI Foundation::Core Foundation::Fonts Foundation::VG)

target_sources(GUI.Inspector
    PUBLIC
        FILE_SET guiinspector_modules TYPE CXX_MODULES FILES
            InspectorModule.cppm
            Queries.cppm
            Inspector.cppm)
