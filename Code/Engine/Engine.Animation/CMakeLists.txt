# Engine::Animation - `engine.animation`, the scene side of skeletal
# animation (SkeletalAnimationComponent + manager + AnimationSubsystem). Sits at the
# animation<->render seam: depends on Animation + Scene + RenderSubsystem (one-way).
util_add_engine_library(Engine.Animation ALIAS Engine::Animation)
target_link_libraries(Engine.Animation
    PUBLIC Foundation::Core Foundation::Runtime Foundation::Scene Engine::Scene
           Foundation::Animation Foundation::Resource Engine::Render Foundation::Script.Facades
           Foundation::PropertyAnimation Foundation::PropertyAnimation.Resource
           Foundation::Profiler Engine::Domain Foundation::Animation.Resource Foundation::Policy)

target_sources(Engine.Animation
    PRIVATE
        AnimationSubsystemImpl.cpp   # REFLECT_VALUE bodies (see AnimationComponents.cppm)
    PUBLIC
        FILE_SET animation_subsystem_modules TYPE CXX_MODULES FILES
            AnimationComponents.cppm
            PropertyAnimatorComponent.cppm
            IkComponents.cppm
            AnimationSubsystem.cppm
            AnimationSubsystemModule.cppm)

