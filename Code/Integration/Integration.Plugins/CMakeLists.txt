# Integration.Plugins - a project's own plugin package in every host: the reference game's
# (Data/SampleProjects/NativeSample, all four parts), built in this tree so every lane compiles it,
# linked into this suite the way a host links its plugins, and loaded the way the editor, the cook
# and the MCP host each load a project's plugins. A build that adds a project's packages
# (ENGINE_PROJECT_PLUGINS) adds its own copy of them, so this suite stays out of it.
util_add_plugin(${CMAKE_SOURCE_DIR}/Data/SampleProjects/NativeSample/Plugins/NativeSample)

add_executable(Integration.Plugins TestsMain.cpp NativeSampleTests.cpp)
util_link_plugins(Integration.Plugins ROLE editor PLUGINS nativesample) # every part an editor loads
target_link_libraries(Integration.Plugins PRIVATE
    Editor::App Editor::Mcp Editor::Project Editor::Core Engine::DefaultApp Engine::Composition Engine::Project
    Pipeline::Core Pipeline::Importer Pipeline::Registration Foundation::Mcp Foundation::Json Foundation::Content
    Foundation::Scene Foundation::Runtime Foundation::Runtime.Client Foundation::Core doctest::doctest)
target_compile_definitions(Integration.Plugins PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.Plugins PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.Plugins COMMAND Integration.Plugins)
