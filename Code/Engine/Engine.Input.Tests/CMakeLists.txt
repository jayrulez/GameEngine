# Engine::Input unit tests (doctest): the InputSubsystem (per-surface scene binding and
# source overrides) and the per-context Input facade. Kept apart from Foundation/Input.Tests so
# the Foundation input tests don't link Engine. Includes the shared synthetic-device helpers from
# the foundation test dir (header-only test support, no link dependency).

add_executable(Engine.Input.Tests
    TestsMain.cpp
    InputEngineTests.cpp)

target_link_libraries(Engine.Input.Tests PRIVATE
    Engine::Input Foundation::Input Foundation::Shell Foundation::Settings
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU} doctest::doctest)

target_include_directories(Engine.Input.Tests PRIVATE
    ${CMAKE_SOURCE_DIR}/Code/Foundation/Input.Tests)  # InputTestSupport.h

target_compile_definitions(Engine.Input.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Engine.Input.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Engine.Input.Tests COMMAND Engine.Input.Tests)
