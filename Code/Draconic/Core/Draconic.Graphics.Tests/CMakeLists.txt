# Draconic::Graphics unit tests (doctest, headless via the Null RHI backend).

add_executable(Draconic.Graphics.Tests
    TestsMain.cpp
    GraphicsTests.cpp)

target_link_libraries(Draconic.Graphics.Tests PRIVATE
    Draconic::Graphics Draconic::Graphics.Null Draconic::Shell.Null doctest::doctest)

target_compile_definitions(Draconic.Graphics.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Graphics.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Graphics.Tests COMMAND Draconic.Graphics.Tests)
