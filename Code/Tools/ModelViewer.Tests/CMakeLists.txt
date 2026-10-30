# ModelViewer unit tests (doctest): a model file opened into the cache and bound, its numbers, the
# cache's reuse, and the model put into a scene. No GPU.

add_executable(ModelViewer.Tests
    TestsMain.cpp
    ModelLibraryTests.cpp)

target_link_libraries(ModelViewer.Tests PRIVATE
    Tools::ModelViewer Engine::Model Engine::Render Engine::Animation Foundation::Scene
    Foundation::VFS Foundation::Model Foundation::Image Foundation::Image.IO doctest::doctest)

target_compile_definitions(ModelViewer.Tests PRIVATE
    DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)

target_compile_options(ModelViewer.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings;-Wno-error=c2y-extensions>"
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")

add_test(NAME ModelViewer.Tests COMMAND ModelViewer.Tests)
