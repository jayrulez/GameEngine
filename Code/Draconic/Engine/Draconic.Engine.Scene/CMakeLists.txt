# Draconic::Engine.Scene - `draconic.engine.scene` (the Context-level scene driver +
# ISceneAware broker). Bridges draconic.scene and draconic.runtime; keeps scene core
# runtime-free.
add_library(Draconic.Engine.Scene STATIC)
add_library(Draconic::Engine.Scene ALIAS Draconic.Engine.Scene)

target_link_libraries(Draconic.Engine.Scene
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Policy)

target_sources(Draconic.Engine.Scene
    PUBLIC
        FILE_SET scene_subsystem_modules TYPE CXX_MODULES FILES
            SceneSubsystem.cppm)

