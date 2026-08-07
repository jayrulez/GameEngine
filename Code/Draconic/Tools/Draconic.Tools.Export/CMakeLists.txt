# Draconic.Tools.Export - packages a project into a shippable dist (cook -> stage scenes -> Content.pak
# + player.xml + Draconic.Engine.Player). See docs/design/roadmap.md "Milestone: MVP-to-Export".
add_executable(Draconic.Tools.Export Main.cpp)
add_dependencies(Draconic.Tools.Export Draconic.Engine.Player)   # stages the player exe when present
target_link_libraries(Draconic.Tools.Export PRIVATE
    Draconic::Foundation Draconic::Scene.Resource
    Draconic::Editor Draconic::Editor.Cook Draconic::Editor.Core
    Draconic::Texture.Editor Draconic::Fonts.Editor Draconic::Image.Editor Draconic::Image.Resource Draconic::Geometry.Editor
    Draconic::Animation.Editor Draconic::Materials.Editor Draconic::Shaders.Editor
    Draconic::Particles.Editor Draconic::Input Draconic::Input.Resource Draconic::Input.Editor Draconic::ModelImporter
    Draconic::Engine.Render Draconic::Engine.Animation Draconic::Engine.Particles
    Draconic::Physics Draconic::Physics.Resource Draconic::Physics.Editor Draconic::Engine.Physics
    Draconic::Audio Draconic::Audio.Resource Draconic::Audio.Editor
    Draconic::Script Draconic::Script.Wren Draconic::Script.AngelScript Draconic::Script.Resource Draconic::Script.Facades
    Draconic::Script.Editor Draconic::Script.Wren.Editor Draconic::Script.AngelScript.Editor
    Draconic::UI.Resource Draconic::UI.Editor)

