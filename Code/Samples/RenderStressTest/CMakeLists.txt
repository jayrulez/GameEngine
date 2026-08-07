# RenderStressTest - a worst-case renderer benchmark (grid of all-visible spheres, shared/unique
# materials, sin-wave bob) we use to keep performance honest as rendering features land. Same
# dependency set as the Sandbox: desktop shell + Vulkan GPU factory + default-app/scene/render.

add_executable(RenderStressTest main.cpp)

target_link_libraries(RenderStressTest PRIVATE
    Draconic::Foundation
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Shell.Desktop Draconic::Runtime.Desktop
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Engine.DefaultApp
    Draconic::Scene
    Draconic::Engine.Scene
    Draconic::Engine.Render
    Draconic::Imgui
    Draconic::Geometry
    Draconic::Materials
    Draconic::Policy)
draconic_copy_sdl3_dll(RenderStressTest)
