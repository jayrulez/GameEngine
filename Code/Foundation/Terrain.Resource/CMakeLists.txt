# Foundation::Terrain.Resource - `foundation.terrain.resource`: the cooked terrain resource (a bundle
# of references to a heightfield, a splatmap and per-layer albedo, resolved through the manager). What
# Ref<Terrain> resolves to for the TerrainComponent.
util_add_engine_library(Terrain.Resource ALIAS Foundation::Terrain.Resource)
target_link_libraries(Terrain.Resource
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content
           Foundation::Heightfield Foundation::Texture.Resource Foundation::Policy)

target_sources(Terrain.Resource
    PUBLIC
        FILE_SET terrain_resource_modules TYPE CXX_MODULES FILES
            Splatmap.cppm
            TerrainResource.cppm)
