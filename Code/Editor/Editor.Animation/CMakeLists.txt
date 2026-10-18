# Editor::Animation - `editor.animation`: the animation graph, clip and skeleton pages.
# Editor exe only.
util_add_engine_library(Editor.Animation ALIAS Editor::Animation)
target_link_libraries(Editor.Animation PUBLIC
    Foundation::Core Foundation::Fonts Foundation::Content Foundation::Resource Foundation::Settings Foundation::RHI
    Foundation::Shell Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render Foundation::Geometry
    Foundation::Animation Foundation::Animation.Resource Pipeline::Animation
    Foundation::UI Foundation::UI.Runtime Foundation::UI.Toolkit Foundation::UI.Viewport
    Foundation::VG.Renderer Editor::Core Editor::App Editor::Preview Foundation::Policy)
target_sources(Editor.Animation
    PRIVATE
        AnimationGraphPageImpl.cpp
        AnimationClipPageImpl.cpp
        SkeletonPageImpl.cpp
    PUBLIC
        FILE_SET editor_animation_modules TYPE CXX_MODULES FILES
            AnimationGraphPage.cppm
            AnimationClipPage.cppm
            SkeletonPage.cppm
            EditorAnimationModule.cppm)
