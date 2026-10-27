add_executable(Editor.Vegetation.Tests TestsMain.cpp VegetationPaintToolTests.cpp VegetationScatterToolTests.cpp VegetationMaskPageTests.cpp)
target_link_libraries(Editor.Vegetation.Tests PRIVATE
    Editor::Vegetation Editor::Core Editor::ViewportTools Editor::App
    Pipeline::Vegetation Pipeline::Core
    Foundation::Vegetation.Resource Foundation::Vegetation Engine::Vegetation
    Foundation::Terrain.Resource Engine::Terrain Foundation::Geometry Foundation::Physics Engine::Physics
    Foundation::Heightfield Foundation::GUI Foundation::GUI.Toolkit Foundation::Image Foundation::Image.IO Foundation::Render
    Foundation::Scene Foundation::Content Foundation::Resource Foundation::VFS doctest::doctest)
target_compile_definitions(Editor.Vegetation.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
    RAPTOR_SOURCE_DIR="${CMAKE_SOURCE_DIR}") # the page's markup the tests load
target_compile_options(Editor.Vegetation.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Vegetation.Tests COMMAND Editor.Vegetation.Tests)
