# Draconic::Script.Editor - `draconic.script.editor` (script class asset/builder/importer, tooling).
# Backend-neutral by construction (B3): links Draconic::Script for the REGISTRY only -
# never a concrete backend library (entry points register backends).
add_library(Draconic.Script.Editor STATIC)
add_library(Draconic::Script.Editor ALIAS Draconic.Script.Editor)
target_link_libraries(Draconic.Script.Editor PUBLIC
    Draconic::Foundation Draconic::Editor Draconic::Editor.Core Draconic::Content
    Draconic::Script Draconic::Script.Resource Draconic::Policy)
target_sources(Draconic.Script.Editor
    PRIVATE
        ScriptAssetImpl.cpp
    PUBLIC FILE_SET script_editor_modules TYPE CXX_MODULES FILES ScriptAsset.cppm)

