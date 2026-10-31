# Engine::Particles unit tests (doctest; the scene and resource integration, a component's
# resource-ref round-trip). These exercise the engine particle component, so they live in the
# Engine collection rather than in Foundation/Particles.Tests (Foundation must not link Engine).

add_executable(Engine.Particles.Tests
    TestsMain.cpp
    ResourceRefTests.cpp)

target_link_libraries(Engine.Particles.Tests PRIVATE
    Engine::Particles Engine::Render Foundation::Particles Foundation::Particles.Resource
    Foundation::Scene Foundation::Scene.Resource Foundation::Resource Foundation::VFS Foundation::Content doctest::doctest)

target_compile_definitions(Engine.Particles.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Engine.Particles.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Particles.Tests COMMAND Engine.Particles.Tests)
