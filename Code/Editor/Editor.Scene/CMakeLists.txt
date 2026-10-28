# Editor::Scene - `editor.scene`: the scene editor plugin (SceneEditorPage +
# factory + RegisterSceneEditor). Linked by the editor EXECUTABLE only (never by editor core/app).
# On the GUI (Documentation/Specs/editor-gui.md, 5.3).

util_add_engine_library(Editor.Scene ALIAS Editor::Scene)
target_link_libraries(Editor.Scene
    PUBLIC
        Foundation::Core Foundation::Content Foundation::RHI Foundation::Shell Foundation::Graphics
        Foundation::Runtime Foundation::Runtime.Client Engine::DefaultApp
        Foundation::Scene Engine::Scene Foundation::Scene.Resource
        Foundation::Render Engine::Render Foundation::Animation Engine::Animation
        Foundation::Animation.Resource Pipeline::Animation
        Foundation::PropertyAnimation.Resource
        Pipeline::ModelImporter Pipeline::Scene Foundation::Texture.Resource
        Foundation::Particles Foundation::Particles.Resource Pipeline::Particles Engine::Particles
        Foundation::Physics Foundation::Physics.Resource Engine::Physics Foundation::Heightfield Foundation::Terrain.Resource Foundation::Vegetation.Resource
        Engine::Navigation Foundation::Navigation.Resource Editor::Navigation
        Engine::Spline Foundation::Spline
        Engine::GUI # the game's GUI textures, live in an editing viewport
        Foundation::Messaging Foundation::GUI Foundation::GUI.Hosting Foundation::GUI.Viewport Editor::App # the page on the GUI
        Foundation::Audio Foundation::Audio.Resource Engine::Audio
        Foundation::Geometry Foundation::Geometry.Resource Pipeline::Geometry
        Foundation::Materials Foundation::Materials.Resource Pipeline::Materials
        Foundation::Shaders Foundation::Shaders.Resource
        Pipeline::Render # the render profile assets (SettingsProfileAsset)
        Foundation::Script Foundation::Script.Resource Foundation::Script.Facades Engine::Script
        Foundation::Input Foundation::Input.Resource Engine::Input
        Engine::Composition # the full manager set for the export's scratch scenes
        Editor::Core Editor::Camera Editor::ViewportTools Editor::PropertyAnimation Foundation::Policy
        Foundation::Mcp Foundation::Json # the scene editor's MCP tools
        Foundation::Xml Foundation::Xml.Serialization) # the inspector's text format (its undo)

target_sources(Editor.Scene
    PRIVATE
        EditContextImpl.cpp
        GizmoImpl.cpp
        ToolsImpl.cpp
        ComponentGizmosImpl.cpp
        AssetThumbnailsImpl.cpp
        ScenePageImpl.cpp
        SceneTreeImpl.cpp
        HierarchyViewImpl.cpp
        InspectorViewImpl.cpp
        EntityRefEditorImpl.cpp
        ScriptBehaviorsSourceImpl.cpp
        SceneSettingsSourceImpl.cpp
        SettingsExtraSourcesImpl.cpp
        SceneHudImpl.cpp
        SceneCameraPreviewImpl.cpp
        SceneToolsImpl.cpp
        EntityJsonImpl.cpp
        SceneMcpToolsImpl.cpp
        SceneMcpEditToolsImpl.cpp
        SettingsProfilesImpl.cpp
        SceneLoadingImpl.cpp
    PUBLIC
        FILE_SET editorscene_modules TYPE CXX_MODULES FILES
            SceneViewSettings.cppm
            ZoomReadout.cppm
            ViewGizmo.cppm
            SceneGrid.cppm
            CameraPreview.cppm
            EditContext.cppm
            SettingsProfiles.cppm
            SceneEditorPageInterface.cppm
            ViewportCaptureRecorder.cppm
            SceneLoading.cppm
            SceneActions.cppm
            EntityJson.cppm
            SceneMcpTools.cppm
            Gizmo.cppm
            Tools.cppm
            ComponentGizmos.cppm
            AssetThumbnails.cppm
            SceneServices.cppm
            SceneOverlays.cppm
            SceneViewState.cppm
            SceneSimulation.cppm
            PrefabFlows.cppm
            SceneTree.cppm
            HierarchyView.cppm
            EntityPickerWindow.cppm
            EntityRefEditor.cppm
            BoneNameEditor.cppm
            CollisionMatrixEditor.cppm
            InspectorExtras.cppm
            ScriptBehaviorsSource.cppm
            SceneSettingsSource.cppm
            SettingsExtraSources.cppm
            BuiltinInspectorExtras.cppm
            InspectorView.cppm
            SceneBar.cppm
            SceneHud.cppm
            SceneCameraPreview.cppm
            SceneTools.cppm
            SceneViewport.cppm
            ScenePage.cppm
            SceneEditorRegistration.cppm
            EditorSceneModule.cppm)
