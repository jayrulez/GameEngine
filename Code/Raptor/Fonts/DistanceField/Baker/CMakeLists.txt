# Raptor::FontsDFBaker — `raptor.fonts.df.baker` (import-time MSDF atlas generation).
# Links msdfgen; not needed at runtime.

add_library(RaptorFontsDFBaker STATIC)
add_library(Raptor::FontsDFBaker ALIAS RaptorFontsDFBaker)

target_link_libraries(RaptorFontsDFBaker
    PUBLIC  Raptor::Core Raptor::Fonts Raptor::FontsDF Raptor::FontsIO Raptor::FontsTTF Raptor::Policy
    PRIVATE Raptor::MsdfgenCore Raptor::Stb)

# msdfgen_impl.cpp includes msdfgen headers which may use exceptions.
# Suppress -Werror for this TU so msdfgen warnings don't break the build.
target_sources(RaptorFontsDFBaker
    PUBLIC
        FILE_SET fonts_df_baker_modules TYPE CXX_MODULES FILES
            DFFontAtlasBaker.cppm
            DFFonts.cppm
            FontsDFBakerModule.cppm
    PRIVATE
        msdfgen_impl.cpp)

set_source_files_properties(msdfgen_impl.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error>")

if(RAPTOR_BUILD_TESTS)
    add_subdirectory(Tests)
endif()
