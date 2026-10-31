# Editor::Particles - `editor.particles`: the particle effect page on the GUI: the systems as a
# tree, the live preview, and the rows of the chosen system.
util_add_engine_library(Editor.Particles ALIAS Editor::Particles)
target_link_libraries(Editor.Particles PUBLIC
    Foundation::Core Foundation::Content Foundation::Resource
    Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render
    Foundation::Particles Foundation::Particles.Resource Pipeline::Particles Engine::Particles
    Foundation::GUI Foundation::GUI.Toolkit
    Editor::Core Editor::App Editor::Preview Foundation::Policy)
target_sources(Editor.Particles
    PRIVATE
        ParticleEffectPageImpl.cpp
    PUBLIC
        FILE_SET editor_particles_modules TYPE CXX_MODULES FILES
            ParticleEffectPage.cppm
            EditorParticlesModule.cppm)
