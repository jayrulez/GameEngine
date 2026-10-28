add_executable(UI.VFS.Tests
    VfsResourceProviderTests.cpp
    TestsMain.cpp)

target_link_libraries(UI.VFS.Tests PRIVATE
    Foundation::UI.VFS Foundation::VFS doctest::doctest)

target_compile_definitions(UI.VFS.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(UI.VFS.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME UI.VFS.Tests COMMAND UI.VFS.Tests)
