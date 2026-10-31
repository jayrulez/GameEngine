# Foundation::Fonts.Coverage - `foundation.fonts.coverage` (rasterizer-free IFont/IFontAtlas).

util_add_engine_library(Fonts.Coverage ALIAS Foundation::Fonts.Coverage)
target_link_libraries(Fonts.Coverage PUBLIC Foundation::Core Foundation::Fonts Foundation::Policy)

target_sources(Fonts.Coverage
    PUBLIC
        FILE_SET fonts_coverage_modules TYPE CXX_MODULES FILES
            CoverageFonts.cppm)

