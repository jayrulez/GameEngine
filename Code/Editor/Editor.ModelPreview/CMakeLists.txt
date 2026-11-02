# Editor::ModelPreview - `editor.modelpreview`: a model file opened for viewing without a project
# (read by the model loaders, cooked into a cache folder, bound) and its clips driven. It needs
# nothing of the editor's core: the model viewer tool links it, and so does the editor's model
# preview. Never linked by the runtime.

util_add_engine_library(Editor.ModelPreview ALIAS Editor::ModelPreview)
target_link_libraries(Editor.ModelPreview
    PUBLIC
        Foundation::Core Foundation::RHI Foundation::VFS Foundation::Content Foundation::Resource
        Foundation::Model Foundation::Model.IO Foundation::Model.GLTF Foundation::Model.FBX
        Foundation::Model.Resource Foundation::Scene Foundation::Animation Engine::Animation Engine::Composition
        Pipeline::ModelImporter
        Foundation::Policy)

target_sources(Editor.ModelPreview
    PUBLIC
        FILE_SET editor_modelpreview_modules TYPE CXX_MODULES FILES
            ModelPreviewModule.cppm
            ModelLibrary.cppm
            ClipPlayback.cppm)
