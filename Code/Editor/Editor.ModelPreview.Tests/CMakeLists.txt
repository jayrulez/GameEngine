# Editor.ModelPreview unit tests (doctest): a model file opened into its cache and bound, its
# numbers, the cache's reuse, the model put into a scene and its clips driven. No GPU.

add_executable(Editor.ModelPreview.Tests
    TestsMain.cpp
    ModelPreviewTests.cpp)

target_link_libraries(Editor.ModelPreview.Tests PRIVATE
    Editor::ModelPreview Engine::Model Engine::Render Engine::Animation Foundation::Scene
    Foundation::VFS Foundation::Model Foundation::Image Foundation::Image.IO doctest::doctest)

target_compile_definitions(Editor.ModelPreview.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(Editor.ModelPreview.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME Editor.ModelPreview.Tests COMMAND Editor.ModelPreview.Tests)
