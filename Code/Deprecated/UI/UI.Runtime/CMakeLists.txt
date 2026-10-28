# Foundation::UI.Runtime - `foundation.ui.runtime`: the reusable UIHost that draws foundation.ui on the runtime's
# multi-window graphics host (one UIContext / N RootViews / per-window VGContext+VGRenderer+InputSurface).
# Toolkit-free - renders any RootView, shared by games and the editor. The docking/workbench layer lives
# above in foundation.ui.application.
util_add_engine_library(UI.Runtime ALIAS Foundation::UI.Runtime)
target_link_libraries(UI.Runtime
    PUBLIC
        Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System Foundation::Shell
        Foundation::Graphics Foundation::VG Foundation::VG.Renderer Foundation::Fonts Foundation::UI
        Foundation::UI.Shell Foundation::Policy)

target_sources(UI.Runtime
    PUBLIC
        FILE_SET uiruntime_modules TYPE CXX_MODULES FILES
            UIRuntime.cppm)
