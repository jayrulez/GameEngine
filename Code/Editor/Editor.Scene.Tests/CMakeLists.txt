# On the GUI in parts with its library: SceneEditorTests, SettingsProfileTests and
# CollisionMatrixTests also drive the page, the hierarchy and the inspector on foundation.ui, and
# InspectorConditionTests the inspector's condition grammar; they come back with those layers
# (Documentation/Specs/editor-gui.md, 5.3).
add_executable(Editor.Scene.Tests TestsMain.cpp SceneActionsTests.cpp ZoomReadoutTests.cpp ViewGizmoTests.cpp SceneGridTests.cpp EditContextTests.cpp GizmoTests.cpp SelectToolTests.cpp CameraPreviewTests.cpp SceneMcpToolsTests.cpp SceneMcpEditToolsTests.cpp SceneServicesTests.cpp)
target_link_libraries(Editor.Scene.Tests PRIVATE Editor::Scene Editor::Core Foundation::Content Engine::Render Engine::Script Engine::Spline Pipeline::Scene Engine::Animation Engine::Navigation Engine::Physics Foundation::Physics Engine::Vegetation Foundation::Vegetation Pipeline::ModelImporter Foundation::VFS Foundation::Geometry Foundation::Materials Pipeline::Materials Pipeline::Render Foundation::Particles Foundation::Mcp Foundation::Json Foundation::GUI doctest::doctest)
target_compile_definitions(Editor.Scene.Tests PRIVATE DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS)
target_compile_options(Editor.Scene.Tests PRIVATE
    "$<$<CXX_COMPILER_ID:Clang>:-Wno-error=#warnings>" "$<$<CXX_COMPILER_ID:GNU>:-Wno-error=cpp>")
add_test(NAME Editor.Scene.Tests COMMAND Editor.Scene.Tests)
