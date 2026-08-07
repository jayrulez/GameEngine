# Draconic::Fonts.IO unit tests (doctest).

add_executable(Draconic.Fonts.IO.Tests TestsMain.cpp FontIOTests.cpp)
target_link_libraries(Draconic.Fonts.IO.Tests PRIVATE Draconic::Fonts.IO Draconic::Fonts.Baked Draconic::Fonts doctest::doctest)
target_compile_definitions(Draconic.Fonts.IO.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Fonts.IO.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.IO.Tests COMMAND Draconic.Fonts.IO.Tests)
