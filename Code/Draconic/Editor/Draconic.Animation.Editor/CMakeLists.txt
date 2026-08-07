# Draconic::Animation.Editor - `draconic.animation.editor`: authoring assets + builders
# that cook skeletons/clips into the content DB. Tooling only (runtime loads cooked).
add_library(Draconic.Animation.Editor STATIC)
add_library(Draconic::Animation.Editor ALIAS Draconic.Animation.Editor)

target_link_libraries(Draconic.Animation.Editor
    PUBLIC Draconic::Foundation Draconic::Editor Draconic::Content
           Draconic::Animation Draconic::Animation.Resource Draconic::Policy)

target_sources(Draconic.Animation.Editor
    PUBLIC
        FILE_SET animation_editor_modules TYPE CXX_MODULES FILES
            AnimationAsset.cppm)

