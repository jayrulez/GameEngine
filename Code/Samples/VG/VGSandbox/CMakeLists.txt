# VG Sandbox — end-to-end VG stack demo (window + VGRenderer).
add_executable(VGSandbox Main.cpp)
target_link_libraries(VGSandbox PRIVATE
    Raptor::Core Raptor::RHI Raptor::RHIVulkan Raptor::Shaders Raptor::SamplesFramework
    Raptor::Image Raptor::Fonts Raptor::FontsTTF Raptor::FontsDF Raptor::FontsDFBaker
    Raptor::VG Raptor::VGRenderer Raptor::VGSVG)
target_compile_definitions(VGSandbox PRIVATE
    RAPTOR_VG_FONT_PATH="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf")
raptor_copy_sdl3_dll(VGSandbox)
