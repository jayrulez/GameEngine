# Pipeline::Materials unit tests (doctest; content DB, no GPU or DXC).

add_executable(Materials.Pipeline.Tests
    TestsMain.cpp
    MaterialEditorTests.cpp)

target_link_libraries(Materials.Pipeline.Tests PRIVATE
    Pipeline::Materials Foundation::VFS Foundation::Shaders Foundation::Shaders.Resource
    doctest::doctest)

target_compile_definitions(Materials.Pipeline.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Materials.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Materials.Pipeline.Tests COMMAND Materials.Pipeline.Tests)
