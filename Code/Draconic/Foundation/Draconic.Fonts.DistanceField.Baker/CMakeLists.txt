# Draconic::Fonts.DistanceField.Baker - `draconic.fonts.distancefield.baker` (import-time MSDF atlas generation).
# Links msdfgen; not needed at runtime.

add_library(Draconic.Fonts.DistanceField.Baker STATIC)
add_library(Draconic::Fonts.DistanceField.Baker ALIAS Draconic.Fonts.DistanceField.Baker)

target_link_libraries(Draconic.Fonts.DistanceField.Baker
    PUBLIC  Draconic::Foundation Draconic::Fonts Draconic::Fonts.DistanceField Draconic::Fonts.IO Draconic::Fonts.TTF Draconic::Policy
    PRIVATE Draconic::MsdfgenCore Draconic::Stb)

# msdfgen_impl.cpp includes msdfgen headers which may use exceptions.
# Suppress -Werror for this TU so msdfgen warnings don't break the build.
target_sources(Draconic.Fonts.DistanceField.Baker
    PUBLIC
        FILE_SET fonts_df_baker_modules TYPE CXX_MODULES FILES
            DFFontAtlasBaker.cppm
            DFFonts.cppm
            FontsDFBakerModule.cppm
    PRIVATE
        msdfgen_impl.cpp)

set_source_files_properties(msdfgen_impl.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error>")

