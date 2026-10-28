# Editor::Vegetation - `editor.vegetation`: the scene-viewport Paint Vegetation brush
# (VegetationPaintTool, tool id "vegetation.paint": paint / erase / smooth one plane of the terrain's
# vegetation mask, one command per stroke, the mask written back to its source asset on Save, the
# touched chunks regrown live) and its settings panel. The terrain brushes' shape (Editor::Terrain).
# And the vegetation mask's page (Documentation/Specs/editor-gui.md, 5.4, layer 7, part 2).
util_add_engine_library(Editor.Vegetation ALIAS Editor::Vegetation)
target_link_libraries(Editor.Vegetation PUBLIC
    Foundation::Core Foundation::Content
    Foundation::Scene Engine::Scene Foundation::Render
    Foundation::GUI
    Editor::Core Editor::App Editor::ViewportTools
    Pipeline::Vegetation Foundation::Vegetation.Resource Foundation::Vegetation Engine::Vegetation
    Foundation::Terrain.Resource Engine::Terrain
    Foundation::Physics Engine::Physics
    Foundation::Heightfield Foundation::Resource Foundation::Image Foundation::Image.IO Foundation::GUI.Toolkit Foundation::Policy)
target_sources(Editor.Vegetation
    PRIVATE VegetationPickImpl.cpp VegetationPaintToolImpl.cpp VegetationScatterToolImpl.cpp
            VegetationToolPanelsImpl.cpp VegetationMaskPageImpl.cpp
    PUBLIC FILE_SET editorvegetation_modules TYPE CXX_MODULES FILES
        VegetationPick.cppm VegetationPaintTool.cppm VegetationScatterTool.cppm
        VegetationMaskPage.cppm VegetationEditorModule.cppm)
