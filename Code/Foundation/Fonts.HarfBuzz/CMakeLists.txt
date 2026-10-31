# Foundation::Fonts.HarfBuzz - `foundation.fonts.harfbuzz`: a text shaper over HarfBuzz (ligatures,
# joined and contextual forms, mark placement), one implementation of the engine's ITextShaper.
# HarfBuzz stays inside this library: its interface exports an install
# call and a shaper factory, and no HarfBuzz type or header crosses it. Fonts without their file
# fall back to the basic shaper (Fonts.TrueType's).

util_add_engine_library(Fonts.HarfBuzz ALIAS Foundation::Fonts.HarfBuzz)
target_link_libraries(Fonts.HarfBuzz
    PUBLIC Foundation::Core Foundation::Fonts
    PRIVATE Foundation::Fonts.TrueType ThirdParty::HarfBuzz Foundation::Policy)

target_sources(Fonts.HarfBuzz
    PUBLIC
        FILE_SET fonts_harfbuzz_modules TYPE CXX_MODULES FILES
            FontsHarfBuzzModule.cppm
    PRIVATE
        HarfBuzzShaperImpl.cpp)
