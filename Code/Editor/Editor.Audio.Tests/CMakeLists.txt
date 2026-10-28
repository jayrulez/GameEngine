# Editor::Audio tests: the clip thumbnails and the audio pages (the clip, the sound cue, the bus layout).
add_executable(Editor.Audio.Tests TestsMain.cpp AudioThumbnailTests.cpp AudioClipPageTests.cpp SoundCuePageTests.cpp BusLayoutPageTests.cpp)
target_link_libraries(Editor.Audio.Tests PRIVATE
    Editor::Audio Editor::App Editor::Core
    Foundation::Audio Pipeline::Audio Foundation::Content Foundation::VFS Foundation::GUI Foundation::GUI.Toolkit doctest::doctest)
target_compile_definitions(Editor.Audio.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Audio.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Audio.Tests COMMAND Editor.Audio.Tests)
