# Foundation::Vegetation - `foundation.vegetation`: the terrain vegetation model (layer
# parameters, the deterministic per-chunk scatter, the distance-fade math). Pure, with no RHI and
# no scene: it reads a Heightfield, SplatWeights and a TerrainChunk and produces terrain-local
# instance transforms, so it runs headless (tests, the Null device, WebGPU without readback).
# The scene component and the render-data provider live in Engine::Vegetation.

util_add_engine_library(Vegetation ALIAS Foundation::Vegetation)
target_link_libraries(Vegetation PUBLIC
    Foundation::Core Foundation::Heightfield Foundation::Terrain Foundation::Terrain.Resource
    Foundation::Vegetation.Resource Foundation::Policy)

target_sources(Vegetation
    PRIVATE
        ScatterImpl.cpp
    PUBLIC
        FILE_SET vegetation_modules TYPE CXX_MODULES FILES
            ScatterLayer.cppm
            Scatter.cppm
            VegetationModule.cppm)
