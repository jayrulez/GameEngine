add_executable(Draconic.UI.VFS.Tests
    VfsResourceProviderTests.cpp
    TestsMain.cpp)

target_link_libraries(Draconic.UI.VFS.Tests PRIVATE
    Draconic::UI.VFS Draconic::VFS doctest::doctest)

target_compile_definitions(Draconic.UI.VFS.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.UI.VFS.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.UI.VFS.Tests COMMAND Draconic.UI.VFS.Tests)
