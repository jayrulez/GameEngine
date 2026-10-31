# Foundation::Image.DDS - `foundation.image.dds` (DDS container read and write, BCn decode).
# The one place the vendored bcdec decoder is implemented (private, so the header does not leak).
util_add_engine_library(Image.DDS ALIAS Foundation::Image.DDS)
target_link_libraries(Image.DDS PUBLIC Foundation::Image Foundation::Core Foundation::Policy PRIVATE ThirdParty::Bcdec)
target_sources(Image.DDS
    PUBLIC FILE_SET image_dds_modules TYPE CXX_MODULES FILES Dds.cppm
    PRIVATE DdsImpl.cpp DdsDecodeImpl.cpp)
# bcdec is plain C in a C++ TU: keep its idioms out of -Werror.
set_source_files_properties(DdsDecodeImpl.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=sign-compare;-Wno-error=unused-function;-Wno-error=implicit-fallthrough;-Wno-error=old-style-cast;-Wno-error=shadow>")
