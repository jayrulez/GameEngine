add_executable(Draconic.Script.Resource.Tests TestsMain.cpp ScriptResourceTests.cpp)
target_link_libraries(Draconic.Script.Resource.Tests PRIVATE
    Draconic::Script.Resource Draconic::Content Draconic::Resource Draconic::VFS
    doctest::doctest)
target_compile_definitions(Draconic.Script.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Draconic.Script.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Script.Resource.Tests COMMAND Draconic.Script.Resource.Tests)
