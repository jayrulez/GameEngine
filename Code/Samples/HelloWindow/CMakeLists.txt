# HelloWindow - minimal windowed Draconic app: opens an SDL3 window, clears it each
# frame through a GraphicsDevice, runs until closed. Needs the Vulkan GPU factory
# (so the window actually presents) in addition to the desktop shell.

add_executable(HelloWindow main.cpp)

target_link_libraries(HelloWindow PRIVATE
    Draconic::Foundation
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Shell.Desktop Draconic::Runtime.Desktop
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Policy)
draconic_copy_sdl3_dll(HelloWindow)
