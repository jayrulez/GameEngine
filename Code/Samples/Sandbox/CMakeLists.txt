# Sandbox - the running dev harness: a windowed DefaultApplication with a scene we
# grow the engine in. Needs the desktop shell and the GPU factory (to present)
# plus the default-app and scene libraries.

add_executable(Sandbox main.cpp)

target_link_libraries(Sandbox PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Engine::Model # InstantiateModel (the shared spawn of a cooked model)
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Engine::DefaultApp
    Engine::Composition
    Foundation::Scene
    Engine::Scene
    Engine::Render
    Engine::Animation
    Foundation::Particles
    Engine::Particles
    Extensions::Imgui
    Foundation::Image
    Foundation::Image.IO
    Foundation::Geometry
    Foundation::Materials
    Pipeline::ModelImporter
    Foundation::Model
    Foundation::Content
    Foundation::Resource
    Foundation::VFS
    Foundation::Geometry.Resource
    Foundation::Materials.Resource
    Foundation::Texture.Resource
    Pipeline::Texture
    Foundation::Animation
    Foundation::Policy)

util_copy_sdl3_dll(Sandbox)
