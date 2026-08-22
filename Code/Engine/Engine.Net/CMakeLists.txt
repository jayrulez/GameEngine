# Engine::Net - `engine.net`: the once-per-context NetworkSubsystem that
# contributes the NetworkComponentManager to the scene composition + registers replicated-component
# reflection. The scene-integration half of networking; the per-instance endpoint is NetManager.
add_library(Engine.Net STATIC)
add_library(Engine::Net ALIAS Engine.Net)

target_link_libraries(Engine.Net
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Net.Replication Foundation::Policy)

target_sources(Engine.Net
    PUBLIC
        FILE_SET net_subsystem_modules TYPE CXX_MODULES FILES
            NetworkSubsystem.cppm)

