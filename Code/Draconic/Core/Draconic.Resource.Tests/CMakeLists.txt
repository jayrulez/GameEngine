# Draconic::Resource unit tests (doctest).

add_executable(Draconic.Resource.Tests TestsMain.cpp ResourceTests.cpp AsyncResourceTests.cpp)
target_link_libraries(Draconic.Resource.Tests PRIVATE Draconic::Resource Draconic::VFS doctest::doctest)
target_compile_definitions(Draconic.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Resource.Tests COMMAND Draconic.Resource.Tests)
