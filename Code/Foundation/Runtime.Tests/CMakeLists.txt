# Foundation::Runtime unit tests (doctest).

# A real plugin shared object, dlopen'd by the PluginHost::Load test.
add_library(Runtime.TestPlugin MODULE TestPlugin.cpp)
set_target_properties(Runtime.TestPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(Runtime.TestPlugin PRIVATE Foundation::Runtime Foundation::Policy)

add_executable(Runtime.Tests
    TestsMain.cpp
    RuntimeTests.cpp)

target_link_libraries(Runtime.Tests PRIVATE Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Foundation::Scene.Resource # the cross-boundary scene-contribution case
    doctest::doctest)
add_dependencies(Runtime.Tests Runtime.TestPlugin)

target_compile_definitions(Runtime.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    TEST_PLUGIN_PATH="$<TARGET_FILE:Runtime.TestPlugin>")

target_compile_options(Runtime.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

# The cross-boundary identity plugin, only in shared-engine builds (the supported
# plugin model). It links the engine's .so set, so identity and rendezvous are
# single-instance by construction; the test proves it across a real dlopen boundary.
if(ENGINE_SHARED_LIBS)
    add_library(Runtime.CrossPlugin MODULE CrossPlugin.cpp)
    set_target_properties(Runtime.CrossPlugin PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(Runtime.CrossPlugin PRIVATE Foundation::Runtime Foundation::Scene
        Foundation::Policy) # contributes a component manager
    add_dependencies(Runtime.Tests Runtime.CrossPlugin)
    target_compile_definitions(Runtime.Tests PRIVATE
        CROSS_PLUGIN_PATH="$<TARGET_FILE:Runtime.CrossPlugin>")
endif()

add_test(NAME Runtime.Tests COMMAND Runtime.Tests)
