# Pipeline::ModelImporter - `modelimporter`, the top-level model import pipeline.
# Bridges the model loaders (foundation.model IR) to the engine's cooked resources: it
# converts a loaded Model into the geometry/material/texture/animation *Source types
# and cooks them through the editor stack into a content database. This is the only
# library that depends on BOTH foundation.model (the loader IR) and the engine resource
# stack, keeping that dependency out of the lower libraries.
util_add_engine_library(ModelImporter ALIAS Pipeline::ModelImporter)
target_link_libraries(ModelImporter
    PUBLIC
        Foundation::Core Pipeline::Importer Foundation::Policy
        Foundation::Model     Foundation::Model.IO  Foundation::Model.GLTF  Foundation::Model.FBX
        Foundation::Geometry  Foundation::Geometry.Resource     Pipeline::Geometry
        Foundation::Materials Foundation::Materials.Resource Pipeline::Materials
        Foundation::Texture.Resource  Pipeline::Texture  Foundation::Image Foundation::Image.IO
        Foundation::Animation Foundation::Animation.Resource Pipeline::Animation
        Foundation::Content   Foundation::Resource   Pipeline::Core
        Foundation::Model.Resource
        Foundation::Physics   Foundation::Physics.Resource  Pipeline::Physics)

target_sources(ModelImporter
    PRIVATE
        ModelImporterImpl.cpp # the manifest's reflection
    PUBLIC
        FILE_SET modelimporter_modules TYPE CXX_MODULES FILES
            FileImport.cppm
            MeshConvert.cppm
            AnimConvert.cppm
            Cook.cppm
            Load.cppm
            ModelImporterModule.cppm)

