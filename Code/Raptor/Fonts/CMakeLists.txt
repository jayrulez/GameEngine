# Raptor::Fonts — `raptor.fonts` (font/glyph/text-layout types + interfaces).
# Ported from Sedulous.Fonts (excluding Fonts.Resources).

add_library(RaptorFonts STATIC)
add_library(Raptor::Fonts ALIAS RaptorFonts)

target_link_libraries(RaptorFonts PUBLIC Raptor::Core Raptor::Image Raptor::Policy)

target_sources(RaptorFonts
    PUBLIC
        FILE_SET fonts_modules TYPE CXX_MODULES FILES
            Types.cppm
            Interfaces.cppm
            TextUtil.cppm
            FontAtlasTexture.cppm
            NullFontService.cppm
            FontsModule.cppm)

# Sedulous draws each of these as its own library; mirror those module lines.
add_subdirectory(Baked)     # raptor.fonts.baked  — rasterizer-free IFont/IFontAtlas
add_subdirectory(IO)        # raptor.fonts.io     — parser/baker pipeline + FontManager
add_subdirectory(TTF)       # raptor.fonts.ttf    — stb_truetype backend + VFS service
add_subdirectory(Importer)  # raptor.fonts.importer — editor-time TTF -> baked
add_subdirectory(DistanceField)  # raptor.fonts.df — distance-field atlas (+ baker when msdfgen available)

if(RAPTOR_BUILD_TESTS)
    add_subdirectory(Tests)
endif()
