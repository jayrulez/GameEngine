# Draconic::Editor.Scene - `draconic.editor.scene`: the scene editor plugin (SceneEditorPage +
# factory + RegisterSceneEditor). Linked by the editor EXECUTABLE only (never by editor core/app).

add_library(Draconic.Editor.Scene STATIC)
add_library(Draconic::Editor.Scene ALIAS Draconic.Editor.Scene)

target_link_libraries(Draconic.Editor.Scene
    PUBLIC
        Draconic::Foundation Draconic::Content Draconic::RHI Draconic::Shell Draconic::Graphics
        Draconic::Runtime Draconic::Runtime.Client Draconic::Engine.DefaultApp
        Draconic::Scene Draconic::Engine.Scene Draconic::Scene.Resource Draconic::Scene.Editor
        Draconic::Render Draconic::Engine.Render Draconic::Animation Draconic::Engine.Animation
        Draconic::Animation.Resource Draconic::Animation.Editor
        Draconic::ModelImporter Draconic::Texture.Resource
        Draconic::Particles Draconic::Particles.Resource Draconic::Particles.Editor Draconic::Engine.Particles
        Draconic::Physics Draconic::Physics.Resource Draconic::Engine.Physics
        Draconic::UI.Resource Draconic::Engine.UI Draconic::Audio Draconic::Audio.Resource Draconic::Engine.Audio
        Draconic::Geometry Draconic::Geometry.Resource Draconic::Geometry.Editor
        Draconic::Materials Draconic::Materials.Resource Draconic::Materials.Editor
        Draconic::UI Draconic::UI.Runtime Draconic::UI.Viewport Draconic::VG.Renderer
        Draconic::Script Draconic::Script.Wren Draconic::Script.Resource Draconic::Script.Facades Draconic::Engine.Script
        Draconic::Input Draconic::Input.Resource Draconic::Engine.Input
        Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)

target_sources(Draconic.Editor.Scene
    PRIVATE
        EditContextImpl.cpp
        GizmoImpl.cpp
        ComponentGizmosImpl.cpp
        HierarchyViewImpl.cpp
        InspectorViewImpl.cpp
        ScenePageImpl.cpp
        GamePageImpl.cpp
        MaterialPageImpl.cpp
        MeshPageImpl.cpp
        ParticleEffectPageImpl.cpp
        AnimationGraphPageImpl.cpp
        AnimationClipPageImpl.cpp
        SkeletonPageImpl.cpp
    PUBLIC
        FILE_SET editorscene_modules TYPE CXX_MODULES FILES
            EditorCamera.cppm
            CameraPreview.cppm
            EditContext.cppm
            Gizmo.cppm
            ComponentGizmos.cppm
            HierarchyView.cppm
            InspectorView.cppm
            ScenePage.cppm
            GamePage.cppm
            MaterialPage.cppm
            MeshPage.cppm
            ParticleEffectPage.cppm
            AnimationGraphPage.cppm
            AnimationClipPage.cppm
            SkeletonPage.cppm
            ModelPrefab.cppm
            EditorSceneModule.cppm)

