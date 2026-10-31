# Foundation::PropertyAnimation.Resource - `foundation.propertyanimation.resource`: the cooked
# property-animation clip as a runtime resource (a Source, through its Factory, becomes a
# PropertyAnimationClipResource).
util_add_engine_library(PropertyAnimation.Resource ALIAS Foundation::PropertyAnimation.Resource)
target_link_libraries(PropertyAnimation.Resource
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::PropertyAnimation
    PRIVATE Foundation::Policy)

target_sources(PropertyAnimation.Resource
    PUBLIC
        FILE_SET propertyanimation_resource_modules TYPE CXX_MODULES FILES
            PropertyAnimationResource.cppm)
