# Foundation::Render - `foundation.render`, the renderer (scene-agnostic). Consumes extracted
# render data (ExtractedView) and draws it with its GPU passes; it knows nothing about the
# scene. Scene integration lives in Engine.Render, which depends on this one-way.
util_add_engine_library(Render ALIAS Foundation::Render)
target_link_libraries(Render
    PUBLIC Foundation::Core Foundation::RHI Foundation::Render.Api Foundation::RenderGraph Foundation::Geometry Foundation::Materials
           Foundation::Shaders Foundation::Shaders.System Foundation::Materials.PipelineCache Foundation::Profiler Foundation::Policy Foundation::Lod)

target_sources(Render
    PRIVATE
        MeshRendererImpl.cpp   # out-of-line MeshRenderer defs (see MeshRenderer.cppm)
        PipelineImpl.cpp        # out-of-line Pipeline defs (see Pipeline.cppm)
        IBLSystemImpl.cpp       # out-of-line IBLSystem defs (see IBLSystem.cppm)
        ReflectionProbeSystemImpl.cpp
        Debug/DebugDrawPassImpl.cpp
        SsrPassImpl.cpp
        SsgiPassImpl.cpp
        AoPassImpl.cpp
        MsaaResolvePassImpl.cpp
        PickSystemImpl.cpp      # out-of-line PickSystem defs (see PickSystem.cppm)
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
            BindGroupCache.cppm
            TaaPass.cppm
            AoPass.cppm
            MsaaResolvePass.cppm
            PickSystem.cppm
            SsrPass.cppm
        SsgiPass.cppm
            FxaaPass.cppm
        DebugBlitPass.cppm ExposurePass.cppm
            DecalPass.cppm
            Debug/DebugFont.cppm
            Debug/DebugDraw.cppm
            Debug/DebugDrawPass.cppm
            RenderModule.cppm)

