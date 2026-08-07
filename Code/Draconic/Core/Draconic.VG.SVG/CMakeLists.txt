# Draconic::VG.SVG - `draconic.vg.svg` (SVG -> VG: parsers, loader, renderer).
# Ported from Sedulous.VG.SVG. Standalone parsers (no XML lib); deps VG + Fonts.

add_library(Draconic.VG.SVG STATIC)
add_library(Draconic::VG.SVG ALIAS Draconic.VG.SVG)

target_link_libraries(Draconic.VG.SVG PUBLIC Draconic::Foundation Draconic::VG Draconic::Fonts Draconic::Policy)

target_sources(Draconic.VG.SVG
    PUBLIC
        FILE_SET vgsvg_modules TYPE CXX_MODULES FILES
            Types.cppm
            Parsers.cppm
            Loader.cppm
            Renderer.cppm
            SVGModule.cppm)

