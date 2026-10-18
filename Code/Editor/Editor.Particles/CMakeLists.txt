# Editor::Particles - `editor.particles`: the particle effect page (live preview + emitters).
# Editor exe only.
util_add_engine_library(Editor.Particles ALIAS Editor::Particles)
target_link_libraries(Editor.Particles PUBLIC
    Foundation::Core Foundation::Fonts Foundation::Content Foundation::Resource Foundation::RHI
    Foundation::Shell Foundation::Graphics Foundation::Runtime Foundation::Runtime.Client
    Foundation::Scene Engine::Scene Foundation::Render Engine::Render Foundation::Particles
    Foundation::Particles.Resource Pipeline::Particles Engine::Particles Foundation::UI
    Foundation::UI.Runtime Foundation::UI.Toolkit Foundation::UI.Viewport Foundation::VG.Renderer
    Editor::Core Editor::App Editor::Preview Foundation::Policy)
target_sources(Editor.Particles
    PRIVATE
        ParticleEffectPageImpl.cpp
    PUBLIC
        FILE_SET editor_particles_modules TYPE CXX_MODULES FILES
            ParticleEffectPage.cppm
            EditorParticlesModule.cppm)
