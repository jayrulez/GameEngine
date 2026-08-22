# Engine::Scene - `engine.scene` (the Context-level scene driver +
# the scene-composition registry). Bridges foundation.scene and foundation.runtime; keeps
# runtime-free.
add_library(Engine.Scene STATIC)
add_library(Engine::Scene ALIAS Engine.Scene)

target_link_libraries(Engine.Scene
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Foundation::Policy)

target_sources(Engine.Scene
    PUBLIC
        FILE_SET scene_subsystem_modules TYPE CXX_MODULES FILES
            SceneSubsystem.cppm)

