# Foundation::Animation.Resource - `foundation.animation.resource`: skeletons and clips as
# cooked resources (Source -> Factory -> runtime Skeleton/AnimationClip).
util_add_engine_library(Animation.Resource ALIAS Foundation::Animation.Resource)
target_link_libraries(Animation.Resource
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Animation Foundation::Policy)

target_sources(Animation.Resource
    PUBLIC
        FILE_SET animation_resource_modules TYPE CXX_MODULES FILES
            AnimationResource.cppm)

