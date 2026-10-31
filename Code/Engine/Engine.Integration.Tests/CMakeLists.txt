# engine.integration tests: the bridge from physics contacts to scripts (kind mapping and install
# semantics). End-to-end delivery of contacts to behaviors through the same bridge is in the script scene tests.
add_executable(Engine.Integration.Tests TestsMain.cpp ContactBridgeTests.cpp)
target_link_libraries(Engine.Integration.Tests PRIVATE
    Engine::Integration Foundation::Physics Engine::Script doctest::doctest)
target_compile_definitions(Engine.Integration.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.Integration.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.Integration.Tests COMMAND Engine.Integration.Tests)
