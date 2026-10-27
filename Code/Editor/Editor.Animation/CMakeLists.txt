# Editor::Animation - `editor.animation`: the animation pages on the GUI (Documentation/Specs/
# editor-gui.md, 5.4, layer 10): the clip and skeleton pages so far, and what the pages' previews
# share. The graph page's old files stay out of the build until its part.
util_add_engine_library(Editor.Animation ALIAS Editor::Animation)
target_link_libraries(Editor.Animation PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource Foundation::Settings
    Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render Foundation::Geometry
    Foundation::Animation Foundation::Animation.Resource Pipeline::Animation Pipeline::Geometry
    Foundation::GUI Foundation::GUI.Toolkit
    Editor::Core Editor::App Editor::Preview Foundation::Policy)
target_sources(Editor.Animation
    PRIVATE
        AnimationPreviewImpl.cpp
        AnimationClipPageImpl.cpp
        SkeletonPageImpl.cpp
    PUBLIC
        FILE_SET editor_animation_modules TYPE CXX_MODULES FILES
            AnimationPreview.cppm
            AnimationClipPage.cppm
            SkeletonPage.cppm
            EditorAnimationModule.cppm)
