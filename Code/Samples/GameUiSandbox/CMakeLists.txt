# GameUiSandbox - a game's screens on the engine GUI (a menu, a bar, a rolling score, a prompt, toasts),
# pushed onto its screen stack from a DefaultApplication.
add_executable(GameUiSandbox main.cpp)

target_link_libraries(GameUiSandbox PRIVATE
    Foundation::Core
    Foundation::Runtime Foundation::Runtime.Client
    Foundation::Shell.Desktop Foundation::Runtime.Desktop
    Foundation::Graphics Foundation::Graphics.Gpu
    Engine::DefaultApp
    Foundation::Scene Engine::Scene Engine::Render
    Engine::GUI Foundation::GUI Foundation::GUI.Gamekit Foundation::GUI.Resources # the screens, the prompt, the toasts
    Foundation::Input
    Foundation::Policy)
util_copy_sdl3_dll(GameUiSandbox)
