# ScriptPlayground - the draconic.script entity-behaviors P1 consumer proof: cubes driven
# by Wren Mover/Spinner behaviors ticked by the ScriptSubsystem under simulation.
add_executable(ScriptPlayground main.cpp)

target_link_libraries(ScriptPlayground PRIVATE
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
    Draconic::Geometry Draconic::Geometry.Resource
    Draconic::Materials Draconic::Materials.Resource
    Draconic::Script Draconic::Script.Wren Draconic::Script.Resource
    Draconic::Script.Facades Draconic::Engine.Script
    Draconic::Policy)
draconic_copy_sdl3_dll(ScriptPlayground)

target_compile_definitions(ScriptPlayground PRIVATE
    DRACONIC_PLAYGROUND_FONT="${CMAKE_SOURCE_DIR}/Data/Assets/fonts/roboto/Roboto-Regular.ttf")
