# Pipeline::Texture - `texture.pipeline` (tooling: TextureAsset, its builder and importer).
util_add_engine_library(Texture.Pipeline ALIAS Pipeline::Texture)
target_link_libraries(Texture.Pipeline PUBLIC
    Foundation::Core Pipeline::Core Pipeline::Importer Foundation::RHI Foundation::Texture Foundation::Texture.Resource
    Foundation::Image Foundation::Image.IO Foundation::Image.DDS Foundation::Content Foundation::Policy Pipeline::Texture.Compression)
target_sources(Texture.Pipeline
    PRIVATE
        TextureAssetImpl.cpp
        TextureCreatorsImpl.cpp
    PUBLIC FILE_SET texture_editor_modules TYPE CXX_MODULES FILES TextureAsset.cppm)

