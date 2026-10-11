# Engine::Render unit tests (doctest; extraction is pure data, no GPU).

add_executable(Engine.Render.Tests
    ResourceRefTests.cpp
    TestsMain.cpp
    ExtractTests.cpp
    LightAtTests.cpp
    RenderProfileTests.cpp
    SurfaceQueryTests.cpp)

target_link_libraries(Engine.Render.Tests PRIVATE
    Engine::Render Foundation::Geometry.Resource Foundation::Materials.Resource Foundation::Scene.Resource
    Foundation::Resource Foundation::VFS Foundation::Content Foundation::RHI.Null doctest::doctest)

target_compile_definitions(Engine.Render.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Engine.Render.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Render.Tests COMMAND Engine.Render.Tests)
