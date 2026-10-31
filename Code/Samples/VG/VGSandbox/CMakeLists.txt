# VG Sandbox - end-to-end VG stack demo (a window and VGRenderer).
add_executable(VGSandbox Main.cpp)
target_link_libraries(VGSandbox PRIVATE
    Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
    Samples::Framework
    Foundation::Image Foundation::Fonts Foundation::Fonts.TrueType Foundation::Fonts.DistanceField Foundation::Fonts.DistanceField.Baker
    Foundation::VG Foundation::VG.Renderer Foundation::VG.SVG)
util_copy_sdl3_dll(VGSandbox)
