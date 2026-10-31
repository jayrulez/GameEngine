# Foundation::Vegetation.Resource - `foundation.vegetation.resource`: the painted vegetation mask
# (u8 density planes over the terrain's 0..1 footprint UV) as a cooked, referenceable resource
# (Ref<VegetationMask>): the raster, its brush cores (paint, erase, smooth), and the cooked
# source and factory. Shaped like the splat sidecar (Foundation::Terrain.Resource :splatmap), with
# one plane per vegetation layer that chooses Mask placement.

util_add_engine_library(Vegetation.Resource ALIAS Foundation::Vegetation.Resource)
target_link_libraries(Vegetation.Resource
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy)

target_sources(Vegetation.Resource
    PUBLIC
        FILE_SET vegetation_resource_modules TYPE CXX_MODULES FILES
            VegetationMask.cppm
            VegetationResourceModule.cppm)
