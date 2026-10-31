# Pipeline::PropertyAnimation - `propertyanimation.pipeline` (tooling: clip asset and builder).
util_add_engine_library(PropertyAnimation.Pipeline ALIAS Pipeline::PropertyAnimation)
target_link_libraries(PropertyAnimation.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Foundation::PropertyAnimation Foundation::PropertyAnimation.Resource
    PRIVATE Foundation::Policy)

target_sources(PropertyAnimation.Pipeline
    PRIVATE
        PropertyAnimationCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET propertyanimation_pipeline_modules TYPE CXX_MODULES FILES
            PropertyAnimationAsset.cppm)
