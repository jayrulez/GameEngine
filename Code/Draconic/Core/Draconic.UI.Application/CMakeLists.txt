# Draconic::UI.Application - `draconic.ui.application`: the docking / workbench layer.
# RuntimeDockableWindowHost implements the toolkit IDockableWindowHost on the runtime multi-window host
# (IApplicationHost + UIHost + shell geometry). The ONLY UI module that links draconic.ui.toolkit, so
# docking cannot bleed into games or the toolkit-free ui.shell / ui.runtime layers.
add_library(Draconic.UI.Application STATIC)
add_library(Draconic::UI.Application ALIAS Draconic.UI.Application)

target_link_libraries(Draconic.UI.Application
    PUBLIC
        Draconic::Foundation Draconic::Shell Draconic::Graphics Draconic::Runtime.Client
        Draconic::UI Draconic::UI.Toolkit Draconic::UI.Runtime Draconic::Policy)

target_sources(Draconic.UI.Application
    PUBLIC
        FILE_SET uiapplication_modules TYPE CXX_MODULES FILES
            UIApplication.cppm)
