# Pipeline::Registration - `pipeline.registration`: the pipeline's composition root. Its job is to
# link every pipeline module, and it exposes RegisterPipelineTypes, RegisterAllBuilders and
# RegisterAllImporters. Headless (no shell, GPU or editor): the CLI cooker, export packager, editor
# and MCP server all consume it, so it must not depend on Editor::Core.

util_add_engine_library(Pipeline.Registration ALIAS Pipeline::Registration)
target_link_libraries(Pipeline.Registration PUBLIC
    Foundation::Core Foundation::Content Foundation::VFS
    Foundation::Script.Facades
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
    Pipeline::Scene
    Foundation::Policy)

# The GUI's documents and sheets, when the GUI is built.
target_link_libraries(Pipeline.Registration PUBLIC Pipeline::GUI Foundation::GUI.Resources)

target_sources(Pipeline.Registration
    PRIVATE
        RegistrationImpl.cpp
    PUBLIC
        FILE_SET pipeline_registration_modules TYPE CXX_MODULES FILES
            Registration.cppm)
