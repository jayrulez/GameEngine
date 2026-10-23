
add_executable(UI.Pipeline.Tests TestsMain.cpp UIPipelineTests.cpp UIReflectionTests.cpp)
target_link_libraries(UI.Pipeline.Tests PRIVATE
    Pipeline::UI Pipeline::VG Foundation::UI.Resource Foundation::VG.Resource Foundation::UI Pipeline::Core
    Foundation::Content Foundation::Resource Foundation::VFS doctest::doctest)
target_compile_definitions(UI.Pipeline.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(UI.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME UI.Pipeline.Tests COMMAND UI.Pipeline.Tests)
