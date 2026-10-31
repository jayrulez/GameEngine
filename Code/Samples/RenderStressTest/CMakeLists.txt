# RenderStressTest - a worst-case renderer benchmark (grid of all-visible spheres, shared/unique
# materials, sin-wave bob) that keeps performance honest as rendering features are added. Same
# dependency set as the Sandbox: the desktop shell, the GPU factory, default-app, scene and render.

add_executable(RenderStressTest main.cpp)

target_link_libraries(RenderStressTest PRIVATE Foundation::VFS
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Engine::DefaultApp
    Foundation::Scene
    Engine::Scene
    Engine::Render
    Extensions::Imgui
    Foundation::Geometry
    Foundation::Materials
    Foundation::Policy)
util_copy_sdl3_dll(RenderStressTest)
