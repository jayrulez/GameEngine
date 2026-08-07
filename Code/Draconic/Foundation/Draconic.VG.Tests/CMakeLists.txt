
add_executable(Draconic.VG.Tests TestsMain.cpp VGCoreTests.cpp VGPathTests.cpp VGTessellationTests.cpp VGBatchCacheTests.cpp VGContextTests.cpp
    # Ports of Sedulous.VG.Tests (inherit upstream coverage):
    VGCurveUtilsTests.cpp VGStyleTests.cpp VGTriangulatorTests.cpp VGDashGeneratorTests.cpp
    VGPathPortTests.cpp VGPathBuilderTests.cpp VGShapeBuilderTests.cpp
    VGFillTessellatorTests.cpp VGStrokeTessellatorTests.cpp
    VGPathCacheTests.cpp VGContextPortTests.cpp)
target_link_libraries(Draconic.VG.Tests PRIVATE Draconic::VG Draconic::Foundation Draconic::Image Draconic::Fonts doctest::doctest)
target_compile_definitions(Draconic.VG.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.VG.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.VG.Tests COMMAND Draconic.VG.Tests)
