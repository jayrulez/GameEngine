# GameInstanceTests.cpp interleaves AngelScript AND Luau cases (each drives its own real VM), so it
# builds only when BOTH backends are on (a per-backend file split would lift this).
if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
add_executable(Engine.GameInstance.Tests TestsMain.cpp GameInstanceTests.cpp)
target_link_libraries(Engine.GameInstance.Tests PRIVATE
    Engine::GameInstance Foundation::Net Foundation::Net.Manager Foundation::Http Foundation::Http.Script
    Foundation::Input Foundation::Shell ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    Foundation::Scene.Resource Foundation::Content Foundation::Resource Foundation::VFS
    doctest::doctest)
target_compile_definitions(Engine.GameInstance.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.GameInstance.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.GameInstance.Tests COMMAND Engine.GameInstance.Tests)
endif()
