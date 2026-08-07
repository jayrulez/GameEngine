# Draconic::Fonts.Baked unit tests (doctest).

add_executable(Draconic.Fonts.Baked.Tests TestsMain.cpp BakedFontTests.cpp)
target_link_libraries(Draconic.Fonts.Baked.Tests PRIVATE Draconic::Fonts.Baked Draconic::Fonts doctest::doctest)
target_compile_definitions(Draconic.Fonts.Baked.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Fonts.Baked.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.Baked.Tests COMMAND Draconic.Fonts.Baked.Tests)
