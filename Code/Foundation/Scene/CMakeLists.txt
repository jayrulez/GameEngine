# Foundation::Scene - `foundation.scene`, the scene / ECS foundation (entities, transform
# hierarchy, components + per-scene systems, SceneSubsystem). Pure data + logic on
# top of Core; renderer-agnostic.
add_library(Scene STATIC)
add_library(Foundation::Scene ALIAS Scene)

target_link_libraries(Scene
    PUBLIC Foundation::Core Foundation::Resource Foundation::Policy)

target_sources(Scene
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
            SceneComposition.cppm
            SceneModule.cppm)

# The Context-level scene driver + ISceneAware broker (bridges Scene + Runtime). Links only the

