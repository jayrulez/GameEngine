# Foundation::UI.Application - `foundation.ui.application`: the docking / workbench layer.
# RuntimeDockableWindowHost implements the toolkit IDockableWindowHost on the runtime multi-window host
# (IApplicationHost + UIHost + shell geometry). The ONLY UI module that links foundation.ui.toolkit, so
# docking cannot bleed into games or the toolkit-free ui.shell / ui.runtime layers.
util_add_engine_library(UI.Application ALIAS Foundation::UI.Application)
target_link_libraries(UI.Application
    PUBLIC
        Foundation::Core Foundation::Shell Foundation::Graphics Foundation::Runtime.Client
        Foundation::UI Foundation::UI.Toolkit Foundation::UI.Runtime Foundation::Policy)

target_sources(UI.Application
    PUBLIC
        FILE_SET uiapplication_modules TYPE CXX_MODULES FILES
            UIApplication.cppm)
