
add_executable(Draconic.VG.Renderer.Tests TestsMain.cpp VGRendererTests.cpp)
target_link_libraries(Draconic.VG.Renderer.Tests PRIVATE
    Draconic::VG.Renderer Draconic::VG Draconic::RHI Draconic::RHI.Null Draconic::Image Draconic::Foundation doctest::doctest)
target_compile_definitions(Draconic.VG.Renderer.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.VG.Renderer.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.VG.Renderer.Tests COMMAND Draconic.VG.Renderer.Tests)
