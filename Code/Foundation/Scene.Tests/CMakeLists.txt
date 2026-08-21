# Foundation::Scene unit tests (doctest; pure logic, no GPU).

add_executable(Scene.Tests
    TestsMain.cpp
    EntityTests.cpp
    TransformTests.cpp
    ComponentTests.cpp
    SceneSystemTests.cpp
    SceneManagerTests.cpp
    SceneCompositionTests.cpp
    EventBusTests.cpp)

target_link_libraries(Scene.Tests PRIVATE
    Foundation::Scene doctest::doctest)

target_compile_definitions(Scene.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Scene.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Scene.Tests COMMAND Scene.Tests)
