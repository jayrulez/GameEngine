# Foundation::Animation - `foundation.animation`, the skeletal-animation foundation
# (skeleton/pose/clip/sampler/player and the animation graph). Pure logic over Core
# math and containers, with no renderer dependency.
util_add_engine_library(Animation ALIAS Foundation::Animation)
target_link_libraries(Animation
    PUBLIC Foundation::Core Foundation::Policy)

target_sources(Animation
    PUBLIC
        FILE_SET animation_modules TYPE CXX_MODULES FILES
            Easing.cppm
            Skeleton.cppm
            AnimationPose.cppm
            AnimationClip.cppm
            Sampler.cppm
            RootMotion.cppm
            PoseModifier.cppm
            InverseKinematics.cppm
            AnimationPlayer.cppm
            AnimationGraph.cppm
            AnimationModule.cppm)

# The scene-integration library (Engine.Animation) is added from the root CMakeLists after
# Render, because it links Engine::Render, which does not exist this early in configuration.
