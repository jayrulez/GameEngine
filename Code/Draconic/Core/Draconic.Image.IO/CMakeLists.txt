# Draconic::Image.IO - `draconic.image.io` (load/save via stb).
add_library(Draconic.Image.IO STATIC)
add_library(Draconic::Image.IO ALIAS Draconic.Image.IO)
target_link_libraries(Draconic.Image.IO PUBLIC Draconic::Image Draconic::Foundation Draconic::Policy PRIVATE Draconic::Stb)
target_sources(Draconic.Image.IO
    PUBLIC FILE_SET image_io_modules TYPE CXX_MODULES FILES ImageIOModule.cppm
    PRIVATE stb_impl.cpp)
target_compile_options(Draconic.Image.IO PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=missing-field-initializers;-Wno-error=unused-function;-Wno-error=unused-but-set-variable;-Wno-error=sign-compare;-Wno-error=implicit-fallthrough;-Wno-error=type-limits>")
