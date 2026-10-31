# Foundation::Geometry.Resource unit tests (doctest; content DB, no GPU).

add_executable(Geometry.Resource.Tests
    TestsMain.cpp
    MeshResourceTests.cpp)

target_link_libraries(Geometry.Resource.Tests PRIVATE
    Foundation::Geometry.Resource Foundation::VFS doctest::doctest)

target_compile_definitions(Geometry.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Geometry.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Geometry.Resource.Tests COMMAND Geometry.Resource.Tests)
