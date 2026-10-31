# Foundation::Terrain unit tests (doctest; pure data and math, no GPU).

add_executable(Terrain.Tests
    TestsMain.cpp
    TerrainTests.cpp)

target_link_libraries(Terrain.Tests PRIVATE
    Foundation::Terrain Foundation::Heightfield doctest::doctest)

target_compile_definitions(Terrain.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Terrain.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Terrain.Tests COMMAND Terrain.Tests)
