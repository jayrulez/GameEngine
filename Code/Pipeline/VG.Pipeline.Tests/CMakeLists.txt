add_executable(VG.Pipeline.Tests TestsMain.cpp VGPipelineTests.cpp)
target_link_libraries(VG.Pipeline.Tests PRIVATE
    Pipeline::VG Foundation::VG.Resource Pipeline::Core Pipeline::Importer
    Foundation::Content Foundation::Resource Foundation::VFS doctest::doctest)
target_compile_definitions(VG.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(VG.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME VG.Pipeline.Tests COMMAND VG.Pipeline.Tests)
