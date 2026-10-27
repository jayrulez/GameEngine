# PhysicsPlayground - physics consumer proof: crates/trigger/sweeper/raycast shove on
# the Jolt-backed subsystem, drawn via the physics debug wireframes.
add_executable(PhysicsPlayground main.cpp)

target_link_libraries(PhysicsPlayground PRIVATE
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
    Foundation::Physics Foundation::Physics.Resource Engine::Physics
    Foundation::GUI Foundation::GUI.Resources Engine::GUI # the HUD canvas, the nameplate billboard, the kiosk panel
    Extensions::Imgui
    Foundation::Policy)
util_copy_sdl3_dll(PhysicsPlayground)
