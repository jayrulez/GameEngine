# Engine::Scene - `engine.scene` (the application-level scene driver and
# the scene-composition registry). Bridges foundation.scene and foundation.runtime.
util_add_engine_library(Engine.Scene ALIAS Engine::Scene)
target_link_libraries(Engine.Scene
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Foundation::Scene.Resource
           Foundation::Policy) # Scene.Resource: resolving preserved component records
                               # when a contributed manager joins a live scene

# Module implementation unit: the plugin-contribution recorder (one per process).
target_sources(Engine.Scene PRIVATE SceneSubsystemImpl.cpp)

target_sources(Engine.Scene
    PUBLIC
        FILE_SET scene_subsystem_modules TYPE CXX_MODULES FILES
            SceneSubsystem.cppm)

