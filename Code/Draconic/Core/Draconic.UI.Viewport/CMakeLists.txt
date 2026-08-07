# Draconic::UI.Viewport - `draconic.ui.viewport`: ViewportView, a ui::View that hosts 3D-rendered
# content (offscreen color+depth RT, an OnRender callback, displayed via the UI's VGContext as an
# external texture; gated input through a shell::InputSurface). Layered on draconic.ui; toolkit-free.
add_library(Draconic.UI.Viewport STATIC)
add_library(Draconic::UI.Viewport ALIAS Draconic.UI.Viewport)

target_link_libraries(Draconic.UI.Viewport
    PUBLIC Draconic::Foundation Draconic::RHI Draconic::Image Draconic::VG Draconic::VG.Renderer Draconic::UI Draconic::Shell)

target_sources(Draconic.UI.Viewport
    PUBLIC
        FILE_SET uiviewport_modules TYPE CXX_MODULES FILES
            ViewportView.cppm)

