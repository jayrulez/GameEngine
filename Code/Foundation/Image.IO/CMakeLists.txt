# Foundation::Image.IO - `foundation.image.io` (load/save via stb).
util_add_engine_library(Image.IO ALIAS Foundation::Image.IO)
target_link_libraries(Image.IO PUBLIC Foundation::Image Foundation::Image.DDS Foundation::Core Foundation::Policy PRIVATE ThirdParty::Stb)
target_sources(Image.IO
    PUBLIC FILE_SET image_io_modules TYPE CXX_MODULES FILES ImageIOModule.cppm
    PRIVATE stb_impl.cpp)
target_compile_options(Image.IO PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=missing-field-initializers;-Wno-error=unused-function;-Wno-error=unused-but-set-variable;-Wno-error=sign-compare;-Wno-error=implicit-fallthrough;-Wno-error=type-limits>")
# The stb decoders run optimised in every configuration (except under MSVC, where /O2 conflicts with
# the Debug runtime checks): a Debug editor decodes 4k JPEGs on import and cook, and stb at -O0
# takes seconds per file. Only the vendored bodies are optimised; the module unit stays at the
# build's level.
if(NOT MSVC)
    set_source_files_properties(stb_impl.cpp PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()
