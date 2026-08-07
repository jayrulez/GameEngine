# Draconic::Fonts.Baked - `draconic.fonts.baked` (rasterizer-free IFont/IFontAtlas).
# Ported from Sedulous.Fonts.Baked (deps: Sedulous.Fonts).

add_library(Draconic.Fonts.Baked STATIC)
add_library(Draconic::Fonts.Baked ALIAS Draconic.Fonts.Baked)

target_link_libraries(Draconic.Fonts.Baked PUBLIC Draconic::Foundation Draconic::Fonts Draconic::Policy)

target_sources(Draconic.Fonts.Baked
    PUBLIC
        FILE_SET fonts_baked_modules TYPE CXX_MODULES FILES
            Baked.cppm)

