# GUI Sandbox - first on-screen test of draconic.gui (widgets + CSS on the VG renderer).
add_executable(GUISandbox Main.cpp)
target_link_libraries(GUISandbox PRIVATE
    Draconic::Foundation Draconic::RHI Draconic::RHI.Vulkan Draconic::Shaders Draconic::Shaders.System
    Draconic::SamplesFramework
    Draconic::Shell Draconic::Image Draconic::Fonts Draconic::Fonts.TTF
    Draconic::VG Draconic::VG.Renderer Draconic::GUI Draconic::GUI.Shell)
target_compile_definitions(GUISandbox PRIVATE
    DRACONIC_GUI_FONT_PATH="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf"
    DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")
draconic_copy_sdl3_dll(GUISandbox)
