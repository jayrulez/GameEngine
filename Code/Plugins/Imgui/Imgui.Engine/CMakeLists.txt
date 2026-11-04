# Imgui.Engine (Plugins::Imgui.Engine) - `imgui.engine`, the Dear ImGui debug-UI integration (RHI
# renderer and subsystem): the imgui plugin's one part, declared from its Plugin.xml.
# Links the vendored ThirdParty::ImGui PUBLIC so consumers (apps) get imgui.h and the ImGui:: API directly.
util_add_plugin_part()
target_link_libraries(${PLUGIN_PART}
    PUBLIC Foundation::Core Foundation::RHI Foundation::Runtime Foundation::Runtime.Client Foundation::Shell
           Foundation::Graphics Foundation::Shaders Foundation::Shaders.System
           ThirdParty::ImGui Foundation::Policy)

target_sources(${PLUGIN_PART}
    PRIVATE
        ImguiPart.cpp # the part's entry
    PUBLIC
        FILE_SET imgui_modules TYPE CXX_MODULES FILES
            ImguiRenderer.cppm
            ImguiSubsystem.cppm
            ImguiModule.cppm)
