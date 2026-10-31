
add_executable(VG.Tests TestsMain.cpp VGCoreTests.cpp VGPathTests.cpp VGTessellationTests.cpp VGBatchCacheTests.cpp VGContextTests.cpp
    VGCurveUtilsTests.cpp VGStyleTests.cpp VGTriangulatorTests.cpp VGDashGeneratorTests.cpp
    VGPathPortTests.cpp VGPathBuilderTests.cpp VGShapeBuilderTests.cpp
    VGFillTessellatorTests.cpp VGStrokeTessellatorTests.cpp
    VGPathCacheTests.cpp VGContextPortTests.cpp)
target_link_libraries(VG.Tests PRIVATE Foundation::VG Foundation::Core Foundation::Image Foundation::Fonts doctest::doctest)
target_compile_definitions(VG.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(VG.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME VG.Tests COMMAND VG.Tests)
