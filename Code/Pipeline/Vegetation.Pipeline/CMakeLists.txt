# Pipeline::Vegetation - `vegetation.pipeline`: the VegetationMaskAsset (the painted vegetation
# mask's source envelope: its dimensions and plane count, with the planes in the "densities"
# sidecar), its cook into the VegetationMask resource, and the PNG importer (one density plane
# per channel). Shaped like the splatmap pipeline (Pipeline::Terrain).
util_add_engine_library(Vegetation.Pipeline ALIAS Pipeline::Vegetation)
target_link_libraries(Vegetation.Pipeline PUBLIC
    Foundation::Core Foundation::VFS Pipeline::Core Pipeline::Importer
    Foundation::Image Foundation::Image.IO
    Foundation::Vegetation.Resource Foundation::Content Foundation::Policy)
target_sources(Vegetation.Pipeline
    PRIVATE
        VegetationCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        VegetationMaskAssetImpl.cpp
    PUBLIC FILE_SET vegetation_pipeline_modules TYPE CXX_MODULES FILES VegetationMaskAsset.cppm)
