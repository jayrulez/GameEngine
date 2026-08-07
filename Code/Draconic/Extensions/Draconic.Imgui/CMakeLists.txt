# Draconic::Imgui - `draconic.imgui`, the Dear ImGui debug-UI integration (RHI renderer + subsystem).
# Links the vendored Draconic::ImGui PUBLIC so consumers (apps) get imgui.h + the ImGui:: API directly.
add_library(Draconic.Imgui STATIC)
add_library(Draconic::Imgui ALIAS Draconic.Imgui)

target_link_libraries(Draconic.Imgui
    PUBLIC Draconic::Foundation Draconic::RHI Draconic::Runtime Draconic::Shell
           Draconic::Graphics Draconic::Shaders Draconic::Shaders.System
           Draconic::ImGui Draconic::Policy)

target_sources(Draconic.Imgui
    PUBLIC
        FILE_SET imgui_modules TYPE CXX_MODULES FILES
            ImguiRenderer.cppm
            ImguiSubsystem.cppm
            ImguiModule.cppm)

# Dev-mode shader source root: the ShaderSystemHost compiles the ImGui shaders (imgui.vs/imgui.ps)
# on demand from here when no cooked pack is present (a dist/browser ships shaders.dpak instead).
target_compile_definitions(Draconic.Imgui
    PRIVATE DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")
