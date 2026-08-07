# Draconic::Animation.Resource - `draconic.animation.resource`: skeletons + clips as
# cooked resources (Source -> Factory -> runtime Skeleton/AnimationClip).
add_library(Draconic.Animation.Resource STATIC)
add_library(Draconic::Animation.Resource ALIAS Draconic.Animation.Resource)

target_link_libraries(Draconic.Animation.Resource
    PUBLIC Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Animation Draconic::Policy)

target_sources(Draconic.Animation.Resource
    PUBLIC
        FILE_SET animation_resource_modules TYPE CXX_MODULES FILES
            AnimationResource.cppm)

