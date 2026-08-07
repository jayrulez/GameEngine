# Draconic::Geometry.Resource unit tests (doctest; content DB - no GPU).

add_executable(Draconic.Geometry.Resource.Tests
    TestsMain.cpp
    MeshResourceTests.cpp)

target_link_libraries(Draconic.Geometry.Resource.Tests PRIVATE
    Draconic::Geometry.Resource Draconic::VFS doctest::doctest)

target_compile_definitions(Draconic.Geometry.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Geometry.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Geometry.Resource.Tests COMMAND Draconic.Geometry.Resource.Tests)
