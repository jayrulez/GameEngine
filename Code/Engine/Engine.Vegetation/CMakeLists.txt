# Engine::Vegetation - `engine.vegetation`: terrain vegetation in the scene. A VegetationLayerComponent
# on the terrain entity (or a child of it) describes one layer (grass from the splat, rocks from a
# layer, ...); its manager scatters per terrain chunk (Foundation::Vegetation) on demand, caches the
# sets and emits one MultiMeshRenderData per (layer, chunk) in range; the shared MeshRenderer's
# instanced path draws them, so there is no renderer of its own. The VegetationSubsystem wires the
# manager into RenderSubsystem as a per-scene render-data provider, as the TerrainSubsystem does.
util_add_engine_library(Engine.Vegetation ALIAS Engine::Vegetation)
target_link_libraries(Engine.Vegetation
    PUBLIC
        Foundation::Core Foundation::Scene Foundation::Resource
        Foundation::Geometry Foundation::Materials
        Foundation::Heightfield Foundation::Terrain Foundation::Terrain.Resource
        Foundation::Vegetation.Resource Foundation::Vegetation
        Foundation::Render Engine::Render Engine::Terrain Engine::Scene
        Foundation::Runtime Foundation::Profiler Engine::Domain Foundation::Policy)

target_sources(Engine.Vegetation
    PRIVATE
        VegetationComponentsImpl.cpp
    PUBLIC
        FILE_SET enginevegetation_modules TYPE CXX_MODULES FILES
            VegetationComponents.cppm
            VegetationSubsystem.cppm
            VegetationModule.cppm)
