# PhysicsPlayground - physics P1 consumer proof: crates/trigger/sweeper/raycast shove on
# the Jolt-backed subsystem, drawn via the physics debug wireframes.
add_executable(PhysicsPlayground main.cpp)

target_link_libraries(PhysicsPlayground PRIVATE
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
    Draconic::Physics Draconic::Physics.Resource Draconic::Engine.Physics
    Draconic::UI Draconic::UI.Resource Draconic::Engine.UI
    Draconic::Imgui
    Draconic::Policy)
draconic_copy_sdl3_dll(PhysicsPlayground)

target_compile_definitions(PhysicsPlayground PRIVATE
    DRACONIC_PLAYGROUND_FONT="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf")
