# Draconic::Image - `draconic.image` (CPU-side image data + manipulation).
add_library(Draconic.Image STATIC)
add_library(Draconic::Image ALIAS Draconic.Image)
target_link_libraries(Draconic.Image PUBLIC Draconic::Foundation Draconic::Policy)
target_sources(Draconic.Image
    PRIVATE
        ImageReflectionImpl.cpp
    PUBLIC FILE_SET image_modules TYPE CXX_MODULES FILES
        PixelFormat.cppm
        ImageData.cppm
        Image.cppm
        NineSlice.cppm
        AtlasBuilder.cppm
        ImageModule.cppm)
