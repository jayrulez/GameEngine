
add_executable(Draconic.VG.SVG.Tests TestsMain.cpp
    SVGColorParserTests.cpp SVGTransformParserTests.cpp SVGPathParserTests.cpp
    SVGLoaderTests.cpp)
target_link_libraries(Draconic.VG.SVG.Tests PRIVATE Draconic::VG.SVG Draconic::VG Draconic::Foundation doctest::doctest)
target_compile_definitions(Draconic.VG.SVG.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.VG.SVG.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.VG.SVG.Tests COMMAND Draconic.VG.SVG.Tests)
