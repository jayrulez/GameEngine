# Engine::Terrain - `engine.terrain`. PHASE A: the TerrainComponent (references a cooked TerrainResource)
# + scene integration. PHASE B/C: the GPU height-texture cache + the chunked geo-mipmap TerrainRenderer
# and its application-level subsystem (registered into RenderSubsystem via the generic seam, like particles).
util_add_engine_library(Engine.Terrain ALIAS Engine::Terrain)
target_link_libraries(Engine.Terrain
    PUBLIC
        Foundation::Core Foundation::Scene Foundation::Resource
        Foundation::Terrain Foundation::Terrain.Resource Foundation::Texture.Resource Foundation::Heightfield
        Foundation::RHI Engine::Domain Foundation::Heightfield.Resource Foundation::Policy
        Foundation::Render Engine::Render Engine::Scene
        Foundation::Shaders Foundation::Shaders.System Foundation::Runtime
        Foundation::Profiler)

target_sources(Engine.Terrain
    PRIVATE
        TerrainComponentsImpl.cpp
    PUBLIC
        FILE_SET engineterrain_modules TYPE CXX_MODULES FILES
            HeightTexture.cppm
            HoledChunkMesh.cppm
            SplatTexture.cppm
            TerrainRenderData.cppm
            TerrainComponents.cppm
            TerrainRenderer.cppm
            TerrainSubsystem.cppm
            TerrainModule.cppm)
