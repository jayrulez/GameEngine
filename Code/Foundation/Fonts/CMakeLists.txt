# Foundation::Fonts - `foundation.fonts` (font, glyph and text-layout types and interfaces, plus
# what every backend extends: the parser, baker and cache interfaces, the factories, FontManager).

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

