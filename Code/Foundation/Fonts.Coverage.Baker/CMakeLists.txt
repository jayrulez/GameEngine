# Foundation::Fonts.Coverage.Baker - `foundation.fonts.coverage.baker` (bakes a TTF into a coverage
# atlas at tooling time).

util_add_engine_library(Fonts.Coverage.Baker ALIAS Foundation::Fonts.Coverage.Baker)
target_link_libraries(Fonts.Coverage.Baker
    PUBLIC Foundation::Core Foundation::Fonts Foundation::Fonts.Coverage Foundation::Fonts.TrueType Foundation::Policy)

target_sources(Fonts.Coverage.Baker
    PUBLIC
        FILE_SET fonts_coverage_baker_modules TYPE CXX_MODULES FILES
            FontBaker.cppm)

