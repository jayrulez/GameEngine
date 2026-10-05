# Headless behaviors battery: a real script VM (registered by the TEST - the library
# stays backend-neutral), a real Scene, NO device/window dependencies.
# ScriptSceneTests.cpp interleaves AngelScript AND Luau cases (each drives its own real VM), so it
# builds only when BOTH backends are on. Splitting it into per-backend files would let it
# run in a single-backend config; without that split it is gated to the both-backends configuration.
if(OPTION_ENABLE_ANGELSCRIPT AND OPTION_ENABLE_LUAU)
add_executable(Engine.Script.Tests TestsMain.cpp ScriptSceneTests.cpp)
target_link_libraries(Engine.Script.Tests PRIVATE
    Engine::Script Foundation::Script.Resource Foundation::Scene Foundation::Scene.Resource
    Foundation::Content Foundation::Resource Foundation::VFS
    Foundation::Physics Engine::Physics Engine::Integration
    Engine::Render Foundation::RHI.Null Foundation::Audio Engine::Audio
    Foundation::Animation Engine::Animation
    Foundation::Particles Engine::Particles
    Foundation::Net Foundation::Net.Replication
    Foundation::UI Engine::UI
    Foundation::Spline Engine::Spline
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU} doctest::doctest)
target_compile_definitions(Engine.Script.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Engine.Script.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Engine.Script.Tests COMMAND Engine.Script.Tests)
endif()
