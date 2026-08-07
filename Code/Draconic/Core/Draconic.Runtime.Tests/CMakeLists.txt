# Draconic::Runtime unit tests (doctest).

# A real plugin shared object, dlopen'd by the PluginHost::Load test.
add_library(Draconic.Runtime.TestPlugin MODULE TestPlugin.cpp)
set_target_properties(Draconic.Runtime.TestPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Draconic.Runtime.TestPlugin PRIVATE Draconic::Runtime Draconic::Policy)

add_executable(Draconic.Runtime.Tests
    TestsMain.cpp
    RuntimeTests.cpp)

target_link_libraries(Draconic.Runtime.Tests PRIVATE Draconic::Runtime Draconic::Runtime.Client doctest::doctest)
add_dependencies(Draconic.Runtime.Tests Draconic.Runtime.TestPlugin)

target_compile_definitions(Draconic.Runtime.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_TEST_PLUGIN_PATH="$<TARGET_FILE:Draconic.Runtime.TestPlugin>")

target_compile_options(Draconic.Runtime.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Draconic.Runtime.Tests COMMAND Draconic.Runtime.Tests)
