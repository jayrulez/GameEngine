# Editor::Terrain - `editor.terrain`: the terrain's editing (Documentation/Specs/editor-gui.md, 5.3,
# layer 5, part 3). On the GUI so far: the scene view's brushes, their panels and the splatmap
# thumbnails. The Terrain asset page (TerrainEditorPage*) stays out of these lists, unedited, until
# its port.
util_add_engine_library(Editor.Terrain ALIAS Editor::Terrain)
target_link_libraries(Editor.Terrain PUBLIC
    Foundation::Core Foundation::Content
    Foundation::Runtime Foundation::Runtime.Client Foundation::Graphics
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render
    Foundation::GUI
    Editor::Core Editor::App Editor::ViewportTools
    Pipeline::Terrain Pipeline::Heightfield Foundation::Terrain Foundation::Terrain.Resource Engine::Terrain
    Foundation::Heightfield Foundation::Heightfield.Resource Foundation::Resource
    Foundation::Image Foundation::Image.IO Foundation::Policy)

target_sources(Editor.Terrain
    PRIVATE SculptImpl.cpp SplatToolImpl.cpp HoleToolImpl.cpp ToolPanelsImpl.cpp
    PUBLIC FILE_SET editorterrain_modules TYPE CXX_MODULES FILES
        Sculpt.cppm SplatTool.cppm HoleTool.cppm SplatmapThumbnailGenerator.cppm ToolPanels.cppm EditorTerrainModule.cppm)
