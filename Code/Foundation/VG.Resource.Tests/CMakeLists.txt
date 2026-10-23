add_executable(VG.Resource.Tests TestsMain.cpp VGResourceTests.cpp)
target_link_libraries(VG.Resource.Tests PRIVATE
    Foundation::VG.Resource Foundation::Content Foundation::Resource Foundation::VFS doctest::doctest)
target_compile_definitions(VG.Resource.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(VG.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME VG.Resource.Tests COMMAND VG.Resource.Tests)
