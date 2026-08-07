# Draconic::Engine.GameInstance - `draconic.engine.gameinstance`: a running game as a first-class
# object (scene pairing + script run context + `Game` + instance time scale). Over scene + script.
# See docs/design/game-instance.md.

add_library(Draconic.Engine.GameInstance STATIC)
add_library(Draconic::Engine.GameInstance ALIAS Draconic.Engine.GameInstance)

target_link_libraries(Draconic.Engine.GameInstance PUBLIC
    Draconic::Foundation Draconic::Scene Draconic::Scene.Resource Draconic::Content Draconic::Resource
    Draconic::Script Draconic::Engine.Script Draconic::Script.Facades
    Draconic::Net.Manager Draconic::Input Draconic::Policy)

target_sources(Draconic.Engine.GameInstance
    PUBLIC
        FILE_SET runtime_gameinstance_modules TYPE CXX_MODULES FILES
            GameInstance.cppm
    PRIVATE
        GameInstanceImpl.cpp)

