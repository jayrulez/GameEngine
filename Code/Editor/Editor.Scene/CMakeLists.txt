# Editor::Scene - `editor.scene`: the scene editor plugin (SceneEditorPage +
# factory + RegisterSceneEditor). Linked by the editor EXECUTABLE only (never by editor core/app).

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
        Foundation::UI.Resource Engine::UI Foundation::Audio Foundation::Audio.Resource Engine::Audio
        Foundation::Geometry Foundation::Geometry.Resource Pipeline::Geometry
        Foundation::Materials Foundation::Materials.Resource Pipeline::Materials
        Pipeline::Render # the render profile assets (SettingsProfileAsset)
        Foundation::UI Foundation::UI.Runtime Foundation::UI.Viewport Foundation::VG.Renderer
        Foundation::Script Foundation::Script.Resource Foundation::Script.Facades Engine::Script
        Foundation::Input Foundation::Input.Resource Engine::Input
        Editor::Core Editor::App Editor::Camera Editor::Preview Editor::ViewportTools Editor::PropertyAnimation Foundation::Policy
        Foundation::Mcp Foundation::Json) # the scene editor's MCP tools

target_sources(Editor.Scene
    PRIVATE
        EditContextImpl.cpp
        GizmoImpl.cpp
        ToolsImpl.cpp
        ComponentGizmosImpl.cpp
        AssetThumbnailsImpl.cpp
        HierarchyViewImpl.cpp
        InspectorViewImpl.cpp
        ScenePageImpl.cpp
        EntityJsonImpl.cpp
        SceneMcpToolsImpl.cpp
        SceneMcpEditToolsImpl.cpp
        PieMcpToolsImpl.cpp
        PieRunToolImpl.cpp
        GamePageImpl.cpp
        MaterialPageImpl.cpp
        MeshPageImpl.cpp
        ParticleEffectPageImpl.cpp
        AnimationGraphPageImpl.cpp
        AnimationClipPageImpl.cpp
        SkeletonPageImpl.cpp
        SettingsProfilesImpl.cpp
        SettingsProfilePageImpl.cpp
    PUBLIC
        FILE_SET editorscene_modules TYPE CXX_MODULES FILES
            SceneViewSettings.cppm
            ZoomReadout.cppm
            ViewGizmo.cppm
            CameraPreview.cppm
            EditContext.cppm
            SettingsProfiles.cppm
            SceneEditorPageInterface.cppm
            ViewportCaptureRecorder.cppm
            PieInstancePage.cppm
            PieMcpTools.cppm
            SceneActions.cppm
            EntityJson.cppm
            SceneMcpTools.cppm
            Gizmo.cppm
            Tools.cppm
            ComponentGizmos.cppm
            AssetThumbnails.cppm
            HierarchyView.cppm
            InspectorView.cppm
            EntityPickerDialog.cppm
            ScenePage.cppm
            GamePage.cppm
            GameResolution.cppm
            MaterialPage.cppm
            SettingsProfilePage.cppm
            MeshPage.cppm
            ParticleEffectPage.cppm
            AnimationGraphPage.cppm
            AnimationClipPage.cppm
            SkeletonPage.cppm
            EditorSceneModule.cppm)

