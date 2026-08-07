# Draconic::Render - `draconic.render`, the renderer (scene-agnostic). Consumes extracted
# render data (ExtractedView) and draws it; knows nothing about the scene. This phase
# is the render-data contract; the GPU forward path lands later (adds the RHI dep).
# Scene integration lives in the Subsystem sub-library (one-way dependency -> here).
add_library(Draconic.Render STATIC)
add_library(Draconic::Render ALIAS Draconic.Render)

target_link_libraries(Draconic.Render
    PUBLIC Draconic::Foundation Draconic::RHI Draconic::Render.Api Draconic::RenderGraph Draconic::Geometry Draconic::Materials
           Draconic::Shaders Draconic::Shaders.System Draconic::Materials.PipelineCache Draconic::Profiler Draconic::Policy)

target_sources(Draconic.Render
    PRIVATE
        MeshRendererImpl.cpp   # out-of-line MeshRenderer defs (see MeshRenderer.cppm)
        PipelineImpl.cpp        # out-of-line Pipeline defs (see Pipeline.cppm)
        IBLSystemImpl.cpp       # out-of-line IBLSystem defs (see IBLSystem.cppm)
        ReflectionProbeSystemImpl.cpp
        Debug/DebugDrawPassImpl.cpp
        SsrPassImpl.cpp
        AoPassImpl.cpp
        RenderDataImpl.cpp
        ShadowSystemImpl.cpp
        ClusterSystemImpl.cpp
        SpriteRendererImpl.cpp
        DecalPassImpl.cpp
        TaaPassImpl.cpp
        BloomPassImpl.cpp
        SkyPassImpl.cpp
        TonemapPassImpl.cpp
        Debug/DebugDrawImpl.cpp
    PUBLIC
        FILE_SET render_modules TYPE CXX_MODULES FILES
            RenderData.cppm
            Views.cppm
            ExtractContext.cppm
            Resources.cppm
            Pipeline.cppm
            GpuMesh.cppm
            MeshRenderer.cppm
            SpriteRenderer.cppm
            ClusterSystem.cppm
            TonemapPass.cppm
            ShadowSystem.cppm
            IBLSystem.cppm
            ReflectionProbeSystem.cppm
            SkyPass.cppm
            BloomPass.cppm
            TaaPass.cppm
            AoPass.cppm
            SsrPass.cppm
            FxaaPass.cppm
            DecalPass.cppm
            Debug/DebugFont.cppm
            Debug/DebugDraw.cppm
            Debug/DebugDrawPass.cppm
            RenderModule.cppm)

# Scene integration (draconic.engine.render): the resource- + scene-aware bridge. Links the base
# Runtime + SceneSubsystem + the cook/material stack, all of which climb, so it climbs too - the
