# Draconic.Tools.Cook - the command-line cooker (headless: no shell/GPU).
add_executable(Draconic.Tools.Cook Main.cpp)
target_link_libraries(Draconic.Tools.Cook PRIVATE
    Draconic::Foundation Draconic::Content Draconic::VFS
    Draconic::Editor Draconic::Editor.Core Draconic::Editor.Cook
    Draconic::Texture.Editor Draconic::Fonts.Editor Draconic::Image.Editor Draconic::Geometry.Editor
    Draconic::Animation.Editor Draconic::Materials.Editor Draconic::Shaders.Editor
    Draconic::Particles.Editor Draconic::Input Draconic::Input.Resource Draconic::Input.Editor Draconic::ModelImporter Draconic::Image.Resource
    Draconic::Physics Draconic::Physics.Resource Draconic::Physics.Editor
    Draconic::Audio Draconic::Audio.Resource Draconic::Audio.Editor
    Draconic::Script Draconic::Script.Wren Draconic::Script.AngelScript Draconic::Script.Resource Draconic::Script.Facades
    Draconic::Script.Editor Draconic::Script.Wren.Editor Draconic::Script.AngelScript.Editor
    Draconic::UI.Resource Draconic::UI.Editor)

