# Draconic::Scene unit tests (doctest; pure logic, no GPU).

add_executable(Draconic.Scene.Tests
    TestsMain.cpp
    EntityTests.cpp
    TransformTests.cpp
    ComponentTests.cpp
    SceneSystemTests.cpp
    SceneManagerTests.cpp
    EventBusTests.cpp)

target_link_libraries(Draconic.Scene.Tests PRIVATE
    Draconic::Scene doctest::doctest)

target_compile_definitions(Draconic.Scene.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Draconic.Scene.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Scene.Tests COMMAND Draconic.Scene.Tests)
