# HelloWindow - minimal windowed app: opens an SDL3 window, clears it each
# frame through a GraphicsDevice, runs until closed. Needs the GPU factory
# (so the window actually presents) in addition to the desktop shell.

add_executable(HelloWindow main.cpp)

target_link_libraries(HelloWindow PRIVATE
    Foundation::Core
    Foundation::Runtime
    Foundation::Runtime.Client
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics
    Foundation::Graphics.Gpu
    Foundation::Policy)
util_copy_sdl3_dll(HelloWindow)
