# Draconic::Scene - `draconic.scene`, the scene / ECS foundation (entities, transform
# hierarchy, components + per-scene systems, SceneSubsystem). Pure data + logic on
# top of Foundation; renderer-agnostic.
add_library(Draconic.Scene STATIC)
add_library(Draconic::Scene ALIAS Draconic.Scene)

target_link_libraries(Draconic.Scene
    PUBLIC Draconic::Foundation Draconic::Resource Draconic::Policy)

target_sources(Draconic.Scene
    PRIVATE
        SceneImpl.cpp   # out-of-line Scene member definitions (see Scene.cppm)
    PUBLIC
        FILE_SET scene_modules TYPE CXX_MODULES FILES
            Entity.cppm
            Phase.cppm
            SceneSystem.cppm
            Component.cppm
            Aware.cppm
            EventBus.cppm
            Scene.cppm
            SceneManager.cppm
            SceneModule.cppm)

# The Context-level scene driver + ISceneAware broker (bridges Scene + Runtime). Links only the

