# Editor::Particles tests: the particle effect page over its markup.
add_executable(Editor.Particles.Tests TestsMain.cpp ParticleEffectPageTests.cpp)
target_link_libraries(Editor.Particles.Tests PRIVATE
    Editor::Particles Editor::App Editor::Core Foundation::Content Foundation::Particles Pipeline::Particles
    Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Particles.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Particles.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Particles.Tests COMMAND Editor.Particles.Tests)
