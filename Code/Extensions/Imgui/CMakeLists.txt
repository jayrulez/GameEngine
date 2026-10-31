# Extensions::Imgui - `extensions.imgui`, the Dear ImGui debug-UI integration (RHI renderer and subsystem).
# Links the vendored ThirdParty::ImGui PUBLIC so consumers (apps) get imgui.h and the ImGui:: API directly.
util_add_engine_library(Imgui ALIAS Extensions::Imgui)
target_link_libraries(Imgui
    PUBLIC Foundation::Core Foundation::RHI Foundation::Runtime Foundation::Shell
           Foundation::Graphics Foundation::Shaders Foundation::Shaders.System
           ThirdParty::ImGui Foundation::Policy)

target_sources(Imgui
    PUBLIC
        FILE_SET imgui_modules TYPE CXX_MODULES FILES
            ImguiRenderer.cppm
            ImguiSubsystem.cppm
            ImguiModule.cppm)
