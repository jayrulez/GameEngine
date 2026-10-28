# Editor::Particles - `editor.particles`: the particle effect page on the GUI (Documentation/Specs/
# editor-gui.md, 5.4, layer 8): the systems a tree, the live preview, the chosen system's rows.
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
