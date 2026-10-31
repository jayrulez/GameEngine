# Foundation::Terrain - `foundation.terrain`, the terrain model over a heightfield: the chunk grid and
# quadtree the chunked geo-mipmap renderer draws, per-chunk LOD selection, and splat descriptors.
# Pure data and math on top of Core and Heightfield, no RHI.
util_add_engine_library(Terrain ALIAS Foundation::Terrain)
target_link_libraries(Terrain
    PUBLIC Foundation::Core Foundation::Heightfield Foundation::Lod Foundation::Policy)

target_sources(Terrain
    PUBLIC
        FILE_SET terrain_modules TYPE CXX_MODULES FILES
            Terrain.cppm)
