# Draconic::Scene.Resource unit tests (doctest; in-memory serialize round-trip).

add_executable(Draconic.Scene.Resource.Tests
    TestsMain.cpp
    SceneSerializeTests.cpp)

target_link_libraries(Draconic.Scene.Resource.Tests PRIVATE
    Draconic::Scene.Resource doctest::doctest)

target_compile_definitions(Draconic.Scene.Resource.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Scene.Resource.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Scene.Resource.Tests COMMAND Draconic.Scene.Resource.Tests)
