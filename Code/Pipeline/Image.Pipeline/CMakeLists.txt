# Pipeline::Image - `image.pipeline` (tooling: ImageAsset and its builder).
util_add_engine_library(Image.Pipeline ALIAS Pipeline::Image)
target_link_libraries(Image.Pipeline PUBLIC Foundation::Core Foundation::VFS Pipeline::Core Pipeline::Importer Foundation::Image Foundation::Image.IO Foundation::Image.Resource Foundation::Content Foundation::Policy)
target_sources(Image.Pipeline
    PRIVATE
        ImageAssetImpl.cpp
    PUBLIC FILE_SET image_editor_modules TYPE CXX_MODULES FILES ImageAsset.cppm)
