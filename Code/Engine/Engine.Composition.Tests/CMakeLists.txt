add_executable(Engine.Composition.Tests TestsMain.cpp CompositionTests.cpp SceneExportSupportTests.cpp
    PluginServicesTests.cpp)
target_link_libraries(Engine.Composition.Tests PRIVATE
    Engine::Composition Foundation::Content Foundation::VFS Foundation::Scene Foundation::Resource Foundation::Script.Facades
    Foundation::RHI Foundation::Shaders.System doctest::doctest)
target_compile_definitions(Engine.Composition.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.Composition.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.Composition.Tests COMMAND Engine.Composition.Tests)
