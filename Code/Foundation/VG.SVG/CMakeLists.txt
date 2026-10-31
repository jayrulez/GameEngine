# Foundation::VG.SVG - `foundation.vg.svg` (SVG to VG: parsers, loader, renderer).
# Standalone parsers (no XML lib); depends on VG and Fonts.

util_add_engine_library(VG.SVG ALIAS Foundation::VG.SVG)
target_link_libraries(VG.SVG PUBLIC Foundation::Core Foundation::VG Foundation::Fonts Foundation::Policy)

target_sources(VG.SVG
    PUBLIC
        FILE_SET vgsvg_modules TYPE CXX_MODULES FILES
            Types.cppm
            Parsers.cppm
            Loader.cppm
            Renderer.cppm
            SVGModule.cppm)

