# Editor::Terrain - `editor.terrain`: the terrain's editing (Documentation/Specs/editor-gui.md, 5.3,
# layer 5, part 3, and 5.4, layer 7): the scene view's brushes, their panels, the splatmap
# thumbnails and the Terrain asset page.
util_add_engine_library(Editor.Terrain ALIAS Editor::Terrain)
target_link_libraries(Editor.Terrain PUBLIC
    Foundation::Core Foundation::Content
    Foundation::Runtime Foundation::Runtime.Client Foundation::Graphics
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render
    Foundation::GUI
    Editor::Core Editor::App Editor::ViewportTools Editor::Preview Foundation::GUI.Toolkit
    Pipeline::Core Pipeline::Terrain Pipeline::Heightfield Foundation::Terrain Foundation::Terrain.Resource Engine::Terrain
    Foundation::Heightfield Foundation::Heightfield.Resource Foundation::Resource
    Foundation::Image Foundation::Image.IO Foundation::Texture.Resource Foundation::Policy)

target_sources(Editor.Terrain
    PRIVATE SculptImpl.cpp SplatToolImpl.cpp HoleToolImpl.cpp ToolPanelsImpl.cpp TerrainEditorPageImpl.cpp
    PUBLIC FILE_SET editorterrain_modules TYPE CXX_MODULES FILES
        Sculpt.cppm SplatTool.cppm HoleTool.cppm SplatmapThumbnailGenerator.cppm ToolPanels.cppm TerrainEditorPage.cppm EditorTerrainModule.cppm)
