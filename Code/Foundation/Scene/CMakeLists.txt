# Foundation::Scene - `foundation.scene`, the scene / ECS foundation (entities, transform
# hierarchy, components + per-scene systems, SceneSubsystem). Pure data + logic on
# top of Core; renderer-agnostic.
util_add_engine_library(Scene ALIAS Foundation::Scene)
target_link_libraries(Scene
    PUBLIC Foundation::Core Foundation::Resource Foundation::Messaging Foundation::Run Foundation::Policy)

target_sources(Scene
    PRIVATE
        SceneImpl.cpp   # out-of-line Scene member definitions (see Scene.cppm)
    PUBLIC
        FILE_SET scene_modules TYPE CXX_MODULES FILES
            Entity.cppm
            Phase.cppm
            SceneSystem.cppm
            Component.cppm
            Scene.cppm
            SceneManager.cppm
            FrameTime.cppm
        SceneComposition.cppm
            SceneModule.cppm)

# The application-level scene driver (bridges Scene + Runtime - the scene-composition registry).

