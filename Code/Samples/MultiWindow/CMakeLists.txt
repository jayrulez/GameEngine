# MultiWindow - runtime-host smoke test: two OS windows, each cleared a different
# color through the shared GraphicsDevice. Needs the Vulkan GPU factory + desktop.

add_executable(MultiWindow main.cpp)

target_link_libraries(MultiWindow PRIVATE
    Draconic::Foundation
    Draconic::RHI
    Draconic::Runtime
    Draconic::Runtime.Client
    Draconic::Shell
    Draconic::Shell.Desktop Draconic::Runtime.Desktop
    Draconic::Graphics
    Draconic::Graphics.Gpu
    Draconic::Policy)
draconic_copy_sdl3_dll(MultiWindow)
