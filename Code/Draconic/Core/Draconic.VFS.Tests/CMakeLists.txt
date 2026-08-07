# Draconic::VFS unit tests (doctest).

add_executable(Draconic.VFS.Tests TestsMain.cpp VfsTests.cpp)
target_link_libraries(Draconic.VFS.Tests PRIVATE Draconic::VFS doctest::doctest)
target_compile_definitions(Draconic.VFS.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.VFS.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.VFS.Tests COMMAND Draconic.VFS.Tests)
