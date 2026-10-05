# Integration.Runtime - flows across the engine's runtime domains in one running scene: an
# animator's root motion walking a physics character (Engine.Animation x Engine.Physics, which
# depend on neither each other; they meet through the scene's capabilities).
add_executable(Integration.Runtime TestsMain.cpp RootMotionCharacterTests.cpp)
target_link_libraries(Integration.Runtime PRIVATE
    Engine::Animation Engine::Physics Engine::Render Foundation::Animation Foundation::Physics
    Foundation::Scene Foundation::Core doctest::doctest)
target_compile_definitions(Integration.Runtime PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Integration.Runtime PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Integration.Runtime COMMAND Integration.Runtime)
