# ModelViewer - `modelviewer`: the model viewer tool's headless half (opening a model file into the
# viewer's cache and binding it, its numbers), tested by ModelViewer.Tests; Tools.ModelViewer is
# the window over it (Documentation/Specs/model-viewer.md).

util_add_engine_library(ModelViewer ALIAS Tools::ModelViewer)
target_link_libraries(ModelViewer
    PUBLIC
        Foundation::Core Foundation::RHI Foundation::VFS Foundation::Content Foundation::Resource
        Foundation::Model Foundation::Model.IO Foundation::Model.GLTF Foundation::Model.FBX
        Foundation::Model.Resource Foundation::Scene Foundation::Animation Engine::Animation Engine::Composition Engine::Model
        Pipeline::ModelImporter
        Foundation::Policy)

target_sources(ModelViewer
    PUBLIC
        FILE_SET modelviewer_modules TYPE CXX_MODULES FILES
            ModelViewerModule.cppm
            ModelLibrary.cppm
            ModelTabs.cppm
            ClipPlayback.cppm
            ModelBrowse.cppm
            Shortlist.cppm)
