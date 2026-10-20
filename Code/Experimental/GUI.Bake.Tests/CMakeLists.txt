add_executable(GUI.Bake.Tests
    GpuPictureBakerTests.cpp
    TestsMain.cpp)

target_link_libraries(GUI.Bake.Tests PRIVATE
    Experimental::GUI.Bake Foundation::RHI Foundation::RHI.Null Foundation::VG.Renderer Foundation::VG.SVG Foundation::Image doctest::doctest)

target_compile_definitions(GUI.Bake.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(GUI.Bake.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME GUI.Bake.Tests COMMAND GUI.Bake.Tests)
