# Foundation::Fonts.Bidi - `foundation.fonts.bidi`: the Unicode bidirectional algorithm over
# SheenBidi, one implementation of the engine's IBidiResolver.
# SheenBidi stays inside this library: its interface exports an install call, and no SheenBidi type
# or header crosses it.

util_add_engine_library(Fonts.Bidi ALIAS Foundation::Fonts.Bidi)
target_link_libraries(Fonts.Bidi
    PUBLIC Foundation::Core Foundation::Fonts
    PRIVATE ThirdParty::SheenBidi Foundation::Policy)

target_sources(Fonts.Bidi
    PUBLIC
        FILE_SET fonts_bidi_modules TYPE CXX_MODULES FILES
            FontsBidiModule.cppm
    PRIVATE
        SheenBidiResolverImpl.cpp)
