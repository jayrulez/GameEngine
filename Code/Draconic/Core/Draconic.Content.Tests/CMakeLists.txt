# Draconic::Content unit tests (doctest).

add_executable(Draconic.Content.Tests TestsMain.cpp ContentTests.cpp)
target_link_libraries(Draconic.Content.Tests PRIVATE Draconic::Content Draconic::Xml.Serialization doctest::doctest)
target_compile_definitions(Draconic.Content.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Content.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Content.Tests COMMAND Draconic.Content.Tests)
