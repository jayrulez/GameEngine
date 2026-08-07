# Draconic::Engine.Animation - `draconic.engine.animation`, the scene side of skeletal
# animation (SkeletalAnimationComponent + manager + AnimationSubsystem). Sits at the
# animation<->render seam: depends on Animation + Scene + RenderSubsystem (one-way).
add_library(Draconic.Engine.Animation STATIC)
add_library(Draconic::Engine.Animation ALIAS Draconic.Engine.Animation)

target_link_libraries(Draconic.Engine.Animation
    PUBLIC Draconic::Foundation Draconic::Runtime Draconic::Scene Draconic::Engine.Scene
           Draconic::Animation Draconic::Resource Draconic::Engine.Render Draconic::Script.Facades
           Draconic::Policy)

target_sources(Draconic.Engine.Animation
    PRIVATE
        AnimationSubsystemImpl.cpp   # DRACONIC_REFLECT_VALUE bodies (see AnimationComponents.cppm)
    PUBLIC
        FILE_SET animation_subsystem_modules TYPE CXX_MODULES FILES
            AnimationComponents.cppm
            AnimationSubsystem.cppm
            AnimationSubsystemModule.cppm)

