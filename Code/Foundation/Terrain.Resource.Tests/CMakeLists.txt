# Foundation::Terrain.Resource unit tests (doctest; content DB, no GPU).

add_executable(Terrain.Resource.Tests
    TestsMain.cpp
    TerrainResourceTests.cpp
    SplatmapTests.cpp)

target_link_libraries(Terrain.Resource.Tests PRIVATE
    Foundation::Terrain.Resource
    Foundation::Heightfield.Resource
    Foundation::Heightfield
    Foundation::VFS
    doctest::doctest)

target_compile_definitions(Terrain.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Terrain.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Terrain.Resource.Tests COMMAND Terrain.Resource.Tests)
