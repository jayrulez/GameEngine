# Draconic::Fonts - `draconic.fonts` (font/glyph/text-layout types + interfaces).
# Ported from Sedulous.Fonts (excluding Fonts.Resources).

add_library(Draconic.Fonts STATIC)
add_library(Draconic::Fonts ALIAS Draconic.Fonts)

target_link_libraries(Draconic.Fonts PUBLIC Draconic::Foundation Draconic::Image Draconic::Policy)

target_sources(Draconic.Fonts
    PUBLIC
        FILE_SET fonts_modules TYPE CXX_MODULES FILES
            Types.cppm
            Interfaces.cppm
            TextUtil.cppm
            FontAtlasTexture.cppm
            NullFontService.cppm
            ScaledViews.cppm
            FontsModule.cppm)

