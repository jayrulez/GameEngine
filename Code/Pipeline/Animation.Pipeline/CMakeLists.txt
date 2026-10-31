# Pipeline::Animation - `animation.pipeline`: authoring assets and builders
# that cook skeletons and clips into the content DB. Tooling only (the runtime loads cooked data).
util_add_engine_library(Animation.Pipeline ALIAS Pipeline::Animation)
target_link_libraries(Animation.Pipeline
    PUBLIC Foundation::Core Pipeline::Core Foundation::Content
           Foundation::Animation Foundation::Animation.Resource Foundation::Policy)

target_sources(Animation.Pipeline
    PRIVATE
        AnimationCreatorsImpl.cpp # File > New, asset_create
    PUBLIC
        FILE_SET animation_editor_modules TYPE CXX_MODULES FILES
            AnimationAsset.cppm)

