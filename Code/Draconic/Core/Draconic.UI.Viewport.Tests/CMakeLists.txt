add_executable(Draconic.UI.Viewport.Tests TestsMain.cpp ViewportViewTests.cpp)
target_link_libraries(Draconic.UI.Viewport.Tests PRIVATE
    Draconic::UI.Viewport Draconic::UI Draconic::VG.Renderer Draconic::VG Draconic::Image
    Draconic::RHI Draconic::RHI.Null Draconic::Shell Draconic::Shell.Null Draconic::Foundation doctest::doctest)
target_compile_definitions(Draconic.UI.Viewport.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.UI.Viewport.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.UI.Viewport.Tests COMMAND Draconic.UI.Viewport.Tests)
