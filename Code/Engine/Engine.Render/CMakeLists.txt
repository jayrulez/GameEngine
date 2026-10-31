# Engine::Render - `engine.render`, the scene side of rendering
# (render components, extraction, the RenderSubsystem). Depends on Scene and
# Render (one-way: the renderer never depends back on this or on Scene).
util_add_engine_library(Engine.Render ALIAS Engine::Render)
target_link_libraries(Engine.Render
    PUBLIC Foundation::Core Foundation::Scene Engine::Scene Foundation::Render Foundation::Runtime
           Foundation::Geometry Foundation::Materials Foundation::Shaders Foundation::Shaders.System
           Foundation::Texture.Resource Foundation::Resource Foundation::Script.Facades
           Foundation::Materials.PipelineCache Foundation::Profiler Engine::Domain Foundation::Geometry.Resource Foundation::Model.Resource Foundation::Materials.Resource Foundation::Image.Resource Foundation::Shaders.Resource Foundation::Policy)

target_sources(Engine.Render
    PRIVATE
        RenderComponentsImpl.cpp   # REFLECT_* bodies (see RenderComponents.cppm)
        RenderSubsystemImpl.cpp    # out-of-line RenderSubsystem defs (see RenderSubsystem.cppm)
        ExtractImpl.cpp            # out-of-line scene-extraction defs (see Extract.cppm)
    PUBLIC
        FILE_SET render_subsystem_modules TYPE CXX_MODULES FILES
            RenderComponents.cppm
            RenderProfiles.cppm
            SceneRenderer.cppm
            Extract.cppm
            RenderSubsystem.cppm
            SubsystemModule.cppm)

