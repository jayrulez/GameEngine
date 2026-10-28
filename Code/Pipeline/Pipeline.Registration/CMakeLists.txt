# Pipeline::Registration - `pipeline.registration`: the pipeline's composition root. Links every
# pipeline module (its whole job - the deliberate fan-in point) and exposes RegisterPipelineTypes /
# RegisterAllBuilders / RegisterAllImporters. Headless (no shell/GPU/editor): the CLI cooker, export
# packager, editor, and MCP server all consume it. NO Editor::Core dependency by design.

util_add_engine_library(Pipeline.Registration ALIAS Pipeline::Registration)
target_link_libraries(Pipeline.Registration PUBLIC
    Foundation::Core Foundation::Content Foundation::VFS
    Foundation::Script.Facades Foundation::UI Foundation::UI.Gamekit
    Foundation::Scene Foundation::Scene.Resource
    Pipeline::Core Pipeline::Importer Pipeline::VG Foundation::VG.Resource
    Pipeline::Texture Pipeline::Fonts Pipeline::Image Pipeline::Heightfield Pipeline::Terrain Pipeline::Vegetation Pipeline::Geometry
    Pipeline::Animation Pipeline::PropertyAnimation Pipeline::Materials Pipeline::Shaders Pipeline::Particles
    Foundation::Input Foundation::Input.Resource Pipeline::Input
    Pipeline::ModelImporter Foundation::Image.Resource Foundation::Heightfield.Resource Foundation::Terrain.Resource Foundation::Vegetation.Resource
    Foundation::Physics Foundation::Physics.Resource Pipeline::Physics
    Pipeline::Render
    Foundation::Navigation Foundation::Navigation.Resource Pipeline::Navigation
    Foundation::Audio Foundation::Audio.Resource Pipeline::Audio
    Foundation::Script ${SCRIPTLIB_ANGELSCRIPT} ${SCRIPTLIB_LUAU}
    Foundation::Script.Resource Foundation::Script.Facades
    Pipeline::Script ${SCRIPTLIB_ANGELSCRIPT_PIPELINE} ${SCRIPTLIB_LUAU_PIPELINE}
    Foundation::UI.Resource Pipeline::UI
    Pipeline::Scene
    Foundation::Policy)

# The GUI's documents and sheets, when the GUI is built (gui-lab-evolution-3.md E3).
target_link_libraries(Pipeline.Registration PUBLIC Pipeline::GUI Foundation::GUI.Resources)

target_sources(Pipeline.Registration
    PRIVATE
        RegistrationImpl.cpp
    PUBLIC
        FILE_SET pipeline_registration_modules TYPE CXX_MODULES FILES
            Registration.cppm)
