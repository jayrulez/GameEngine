# Foundation::Fonts - `foundation.fonts` (font/glyph/text-layout types + interfaces, plus the
# backend seam: parser/baker/cache interfaces, factories, FontManager - Sedulous.Fonts.IO,
# folded in 2026-09-05: it is the extension seam every backend needs, not I/O).
# Ported from Sedulous.Fonts (excluding Fonts.Resources).

util_add_engine_library(Fonts ALIAS Foundation::Fonts)
target_link_libraries(Fonts PUBLIC Foundation::Core Foundation::Image Foundation::Policy)

target_sources(Fonts
    PUBLIC
        FILE_SET fonts_modules TYPE CXX_MODULES FILES
            Types.cppm
            Interfaces.cppm
            TextUtil.cppm
            FontAtlasTexture.cppm
            NullFontService.cppm
            ScaledViews.cppm
            BackendInterfaces.cppm
            Factories.cppm
            FontManager.cppm
            Bidi.cppm
            ClusterTextShaper.cppm
            FontsModule.cppm
    PRIVATE
        FontsImpl.cpp)

