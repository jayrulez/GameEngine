# Foundation::Fonts.TrueType - `foundation.fonts.truetype` (stb_truetype backend and VFS service).

util_add_engine_library(Fonts.TrueType ALIAS Foundation::Fonts.TrueType)
target_link_libraries(Fonts.TrueType
    PUBLIC Foundation::Core Foundation::Fonts Foundation::Image Foundation::VFS Foundation::Policy
    PRIVATE ThirdParty::Stb)

target_sources(Fonts.TrueType
    PUBLIC
        FILE_SET fonts_truetype_modules TYPE CXX_MODULES FILES
            Common.cppm
            TrueTypeFont.cppm
            TrueTypeFontAtlas.cppm
            TrueTypeFontParser.cppm
            TrueTypeFontAtlasBaker.cppm
            TrueTypeTextShaper.cppm
            TrueTypeFonts.cppm
            TrueTypeFontService.cppm
            FontsTrueTypeModule.cppm
    PRIVATE
        stb_truetype_impl.cpp)

# stb_truetype.h trips a few warnings the engine treats as errors.
target_compile_options(Fonts.TrueType PRIVATE
    "$<$<CXX_COMPILER_ID:Clang,GNU>:-Wno-error=missing-field-initializers;-Wno-error=unused-function;-Wno-error=unused-but-set-variable;-Wno-error=sign-compare;-Wno-error=implicit-fallthrough;-Wno-error=type-limits;-Wno-error=cast-qual;-Wno-error=double-promotion>")

