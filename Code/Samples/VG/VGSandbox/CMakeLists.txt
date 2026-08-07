# VG Sandbox - end-to-end VG stack demo (window + VGRenderer).
add_executable(VGSandbox Main.cpp)
target_link_libraries(VGSandbox PRIVATE
    Draconic::Foundation Draconic::RHI Draconic::RHI.Vulkan Draconic::Shaders Draconic::Shaders.System
    Draconic::SamplesFramework
    Draconic::Image Draconic::Fonts Draconic::Fonts.TTF Draconic::Fonts.DistanceField Draconic::Fonts.DistanceField.Baker
    Draconic::VG Draconic::VG.Renderer Draconic::VG.SVG)
target_compile_definitions(VGSandbox PRIVATE
    DRACONIC_VG_FONT_PATH="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf"
    DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")
draconic_copy_sdl3_dll(VGSandbox)
