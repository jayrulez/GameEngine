# Draconic::Engine.Render - `draconic.engine.render`, the scene side of rendering
# (render components + extraction + later the RenderSubsystem). Depends on Scene AND
# Render (one-way: the renderer never depends back on this or on Scene).
add_library(Draconic.Engine.Render STATIC)
add_library(Draconic::Engine.Render ALIAS Draconic.Engine.Render)

target_link_libraries(Draconic.Engine.Render
    PUBLIC Draconic::Foundation Draconic::Scene Draconic::Engine.Scene Draconic::Render Draconic::Runtime
           Draconic::Geometry Draconic::Materials Draconic::Shaders Draconic::Shaders.System
           Draconic::Texture.Resource Draconic::Resource Draconic::Script.Facades
           Draconic::Materials.PipelineCache Draconic::Profiler Draconic::Policy)

# Dev builds resolve engine built-in shaders straight from the source tree (edit ->
# hot reload); dists stage them as "Shaders" next to the executable (the fallback).
target_compile_definitions(Draconic.Engine.Render
    PRIVATE DRACONIC_ENGINE_SHADER_DIR="${CMAKE_SOURCE_DIR}/Data/Shaders")

target_sources(Draconic.Engine.Render
    PRIVATE
        RenderComponentsImpl.cpp   # DRACONIC_REFLECT_* bodies (see RenderComponents.cppm)
        RenderSubsystemImpl.cpp    # out-of-line RenderSubsystem defs (see RenderSubsystem.cppm)
        ExtractImpl.cpp            # out-of-line scene-extraction defs (see Extract.cppm)
    PUBLIC
        FILE_SET render_subsystem_modules TYPE CXX_MODULES FILES
            RenderComponents.cppm
            SceneRenderer.cppm
            Extract.cppm
            RenderSubsystem.cppm
            SubsystemModule.cppm)

