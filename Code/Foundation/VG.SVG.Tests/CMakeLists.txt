
add_executable(VG.SVG.Tests TestsMain.cpp
    SVGColorParserTests.cpp SVGTransformParserTests.cpp SVGPathParserTests.cpp
    SVGLoaderTests.cpp SVGIconRasterTests.cpp)
target_link_libraries(VG.SVG.Tests PRIVATE Foundation::VG.SVG Foundation::VG Foundation::Core doctest::doctest)
target_compile_definitions(VG.SVG.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(VG.SVG.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME VG.SVG.Tests COMMAND VG.SVG.Tests)
