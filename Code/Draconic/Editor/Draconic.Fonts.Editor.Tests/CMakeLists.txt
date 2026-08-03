add_executable(Draconic.Fonts.Editor.Tests TestsMain.cpp FontPipelineTests.cpp FontReflectionTests.cpp)
target_link_libraries(Draconic.Fonts.Editor.Tests PRIVATE
    Draconic::Fonts.Editor Draconic::Fonts.Resource Draconic::Fonts Draconic::Editor
    Draconic::Editor.Core Draconic::Content Draconic::Resource Draconic::VFS Draconic::Image doctest::doctest)
target_compile_definitions(Draconic.Fonts.Editor.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    DRACONIC_TEST_FONT_PATH="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/dejavu/DejaVuSansMono.ttf")
target_compile_options(Draconic.Fonts.Editor.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Draconic.Fonts.Editor.Tests COMMAND Draconic.Fonts.Editor.Tests)
