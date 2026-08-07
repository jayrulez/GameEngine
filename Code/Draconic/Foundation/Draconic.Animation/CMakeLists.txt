# Draconic::Animation - `draconic.animation`, the skeletal-animation foundation
# (skeleton/pose/clip/sampler/player + animation graph). A faithful port of
# Sedulous.Animation; pure logic over Foundation math + containers, no renderer dep.
add_library(Draconic.Animation STATIC)
add_library(Draconic::Animation ALIAS Draconic.Animation)

target_link_libraries(Draconic.Animation
    PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Animation
    PUBLIC
        FILE_SET animation_modules TYPE CXX_MODULES FILES
            Easing.cppm
            Skeleton.cppm
            AnimationPose.cppm
            AnimationClip.cppm
            Sampler.cppm
            AnimationPlayer.cppm
            AnimationGraph.cppm
            AnimationModule.cppm)

# NB: the scene-integration lib (Animation/Subsystem) is added from the ROOT CMakeLists AFTER
# Render, because it links Draconic::Engine.Render (which doesn't exist this early in config).
