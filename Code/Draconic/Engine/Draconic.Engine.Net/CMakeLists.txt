# Draconic::Engine.Net - `draconic.engine.net`: the once-per-context NetworkSubsystem that
# injects the NetworkComponentManager into scenes (ISceneAware) + registers replicated-component
# reflection. The scene-integration half of networking; the per-instance endpoint is NetManager.
add_library(Draconic.Engine.Net STATIC)
add_library(Draconic::Engine.Net ALIAS Draconic.Engine.Net)

target_link_libraries(Draconic.Engine.Net
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Net.Replication Draconic::Policy)

target_sources(Draconic.Engine.Net
    PUBLIC
        FILE_SET net_subsystem_modules TYPE CXX_MODULES FILES
            NetworkSubsystem.cppm)

