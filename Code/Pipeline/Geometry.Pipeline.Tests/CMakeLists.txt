# Pipeline::Geometry unit tests (doctest; content DB, no GPU).

add_executable(Geometry.Pipeline.Tests
    TestsMain.cpp
    MeshEditorTests.cpp
    MeshOptimizeTests.cpp
    MeshLodWireTests.cpp)

target_link_libraries(Geometry.Pipeline.Tests PRIVATE
    Pipeline::Geometry Foundation::VFS Foundation::Xml.Serialization doctest::doctest)

target_compile_definitions(Geometry.Pipeline.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Geometry.Pipeline.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Geometry.Pipeline.Tests COMMAND Geometry.Pipeline.Tests)
