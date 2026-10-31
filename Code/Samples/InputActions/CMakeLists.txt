# InputActions - the foundation.input consumer proof: named actions (WASD/stick move, Jump,
# an exclusive Menu set) driving visible state. No renderer: the clear color shows the position.
add_executable(InputActions main.cpp)

target_link_libraries(InputActions PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Foundation::Input Engine::Input
    Foundation::Settings Foundation::Xml.Serialization Extensions::Imgui
    Foundation::Policy)
util_copy_sdl3_dll(InputActions)
