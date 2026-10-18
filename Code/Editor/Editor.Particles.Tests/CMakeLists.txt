# Editor::Particles tests: the particle effect page.
add_executable(Editor.Particles.Tests TestsMain.cpp ParticleEffectPageTests.cpp)
target_link_libraries(Editor.Particles.Tests PRIVATE
    Editor::Particles Editor::Core Foundation::Content Foundation::Particles Pipeline::Particles
    doctest::doctest)
target_compile_definitions(Editor.Particles.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Particles.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Particles.Tests COMMAND Editor.Particles.Tests)
