# InputActions - the foundation.input consumer proof: named actions (WASD/stick move, Jump,
# an exclusive Menu set) driving visible state. No renderer: the clear color shows the position.
add_executable(InputActions main.cpp)

target_link_libraries(InputActions PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Engine::Project
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Foundation::Input Engine::Input
    Foundation::Settings Foundation::Xml.Serialization
    Foundation::Policy)
util_copy_sdl3_dll(InputActions)

# Dear ImGui, which the app loads as a plugin (its part linked, and the registry listing it).
util_link_plugins(InputActions ROLE player PLUGINS imgui)
