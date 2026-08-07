# UI Sandbox - on-screen test of draconic.ui (the Sedulous.UI port) on the VG renderer.
# Ports Sedulous's UISandbox demo (sans the Toolkit tabs) to prove the port end-to-end.
add_executable(UISandbox Main.cpp)
target_link_libraries(UISandbox PRIVATE
    Draconic::Foundation Draconic::RHI Draconic::Shaders
    Draconic::Runtime Draconic::Runtime.Client Draconic::Runtime.Desktop Draconic::Shell.Desktop
    Draconic::Graphics Draconic::Graphics.Gpu Draconic::UI.Runtime Draconic::UI.Application
    Draconic::Shell Draconic::Image Draconic::Fonts Draconic::Fonts.TTF
    Draconic::VG Draconic::VG.Renderer Draconic::UI Draconic::UI.Toolkit Draconic::UI.Viewport Draconic::UI.Shell Draconic::UI.VFS Draconic::VFS)
target_compile_definitions(UISandbox PRIVATE
    DRACONIC_UI_FONT_PATH="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf"
    DRACONIC_UI_ASSET_DIR="${CMAKE_SOURCE_DIR}/Data/Assets")
draconic_copy_sdl3_dll(UISandbox)
