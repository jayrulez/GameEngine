# Pipeline::Script - `script.pipeline` (script class asset/builder/importer, tooling).
# Backend-neutral by construction: links Foundation::Script only for the registry, never a
# concrete backend library (entry points register backends).
util_add_engine_library(Script.Pipeline ALIAS Pipeline::Script)
target_link_libraries(Script.Pipeline PUBLIC
    Foundation::Core Pipeline::Core Pipeline::Importer Foundation::Content
    Foundation::Script Foundation::Script.Resource Foundation::Policy)
target_sources(Script.Pipeline
    PRIVATE
        ScriptCreatorsImpl.cpp # File > New, asset_create
    PRIVATE
        ScriptAssetImpl.cpp
    PUBLIC FILE_SET script_editor_modules TYPE CXX_MODULES FILES ScriptAsset.cppm)

