# InputActions - the draconic.input consumer proof: named actions (WASD/stick move, Jump,
# an exclusive Menu set) driving visible state. No renderer - clear-color = position.
add_executable(InputActions main.cpp)

target_link_libraries(InputActions PRIVATE
    Draconic::Foundation
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Shell.Desktop Draconic::Runtime.Desktop
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Input Draconic::Engine.Input
    Draconic::Settings Draconic::Xml.Serialization Draconic::Imgui
    Draconic::Policy)
draconic_copy_sdl3_dll(InputActions)
