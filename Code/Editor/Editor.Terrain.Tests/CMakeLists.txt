add_executable(Editor.Terrain.Tests TestsMain.cpp SculptToolTests.cpp
    HoleToolTests.cpp SplatToolTests.cpp PanelProviderTests.cpp SplatmapThumbnailTests.cpp) # the asset page's suite returns with it
target_link_libraries(Editor.Terrain.Tests PRIVATE
    Editor::Terrain Editor::Core Editor::ViewportTools Editor::App
    Pipeline::Terrain Pipeline::Heightfield Pipeline::Core
    Foundation::Terrain Foundation::Terrain.Resource
    Foundation::Heightfield Foundation::Heightfield.Resource Foundation::GUI
    Foundation::Scene Engine::Terrain Foundation::Content Foundation::Resource Foundation::VFS
    doctest::doctest)
target_compile_definitions(Editor.Terrain.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Terrain.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Terrain.Tests COMMAND Editor.Terrain.Tests)
