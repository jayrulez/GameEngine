# Engine::Animation unit tests (doctest; components are pure data, no GPU).

add_executable(Engine.Animation.Tests
    ResourceRefTests.cpp
    IkComponentTests.cpp
    PropertyAnimatorTests.cpp
    TestsMain.cpp)

target_link_libraries(Engine.Animation.Tests PRIVATE
    Engine::Animation Foundation::Animation.Resource Foundation::Scene.Resource
    Foundation::PropertyAnimation Foundation::PropertyAnimation.Resource
    Foundation::Resource Foundation::VFS Foundation::Content doctest::doctest)

target_compile_definitions(Engine.Animation.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Engine.Animation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Animation.Tests COMMAND Engine.Animation.Tests)
