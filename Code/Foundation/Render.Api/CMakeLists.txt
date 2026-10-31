# Foundation::Render.Api - `foundation.render.api`: the scene-rendering interface (ISceneRenderer,
# ViewCamera/ViewportRect/CameraOverride/TargetState) in a light module (core, rhi and scene only),
# so tools drive rendering through the interface without linking the renderer. foundation.render
# re-exports it; RenderSubsystem implements it.

util_add_engine_library(Render.Api ALIAS Foundation::Render.Api)
target_link_libraries(Render.Api PUBLIC Foundation::Core Foundation::RHI Foundation::Scene Foundation::Policy)

target_sources(Render.Api
    PUBLIC
        FILE_SET renderapi_modules TYPE CXX_MODULES FILES
            RenderApi.cppm)
