# Draconic::Render.Api - `draconic.render.api`: the scene-rendering interface (ISceneRenderer +
# ViewCamera/ViewportRect/CameraOverride/TargetState) in a LIGHT module (core + rhi + scene only),
# so tools drive rendering through the interface without linking the renderer. draconic.render
# re-exports it; RenderSubsystem implements it.

add_library(Draconic.Render.Api STATIC)
add_library(Draconic::Render.Api ALIAS Draconic.Render.Api)

target_link_libraries(Draconic.Render.Api PUBLIC Draconic::Foundation Draconic::RHI Draconic::Scene Draconic::Policy)

target_sources(Draconic.Render.Api
    PUBLIC
        FILE_SET renderapi_modules TYPE CXX_MODULES FILES
            RenderApi.cppm)
