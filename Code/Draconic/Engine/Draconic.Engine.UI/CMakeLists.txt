# Draconic::Engine.UI - `draconic.engine.ui`: the game screen tier (canvases + overlay render + consumption).
add_library(Draconic.Engine.UI STATIC)
add_library(Draconic::Engine.UI ALIAS Draconic.Engine.UI)
target_link_libraries(Draconic.Engine.UI
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Resource Draconic::Shell Draconic::RHI Draconic::Fonts Draconic::Fonts.TTF Draconic::Fonts.Resource
           Draconic::Input Draconic::Engine.Input Draconic::UI Draconic::UI.Shell
           Draconic::UI.Resource
           Draconic::Script Draconic::Script.Facades
           Draconic::VG Draconic::VG.Renderer Draconic::Shaders Draconic::Shaders.System
           Draconic::Render Draconic::Engine.Render Draconic::Policy)
target_sources(Draconic.Engine.UI
    PRIVATE
        UISubsystemImpl.cpp   # implementation unit: VG/shader contact + reflection bodies
    PUBLIC
        FILE_SET ui_subsystem_modules TYPE CXX_MODULES FILES
            UISubsystem.cppm)

# Dev-mode shader source root: the ShaderSystemHost compiles the VG shaders (vg.vs/vg.ps) on demand
# from here when no cooked pack is present (a dist/browser ships shaders.dpak and needs no compiler).
target_compile_definitions(Draconic.Engine.UI
    PRIVATE DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")

