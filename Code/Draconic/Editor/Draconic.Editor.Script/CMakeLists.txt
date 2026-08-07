# Draconic::Editor.Script - `draconic.editor.script`: the ScriptPage (behavior source text
# editor + save->recook->hot-reload + inline compile errors). Editor exe only.
add_library(Draconic.Editor.Script STATIC)
add_library(Draconic::Editor.Script ALIAS Draconic.Editor.Script)
target_link_libraries(Draconic.Editor.Script PUBLIC
    Draconic::Foundation Draconic::Content Draconic::Runtime.Client Draconic::UI Draconic::UI.Toolkit
    Draconic::Script Draconic::Script.Facades Draconic::Script.Editor Draconic::Script.Resource
    Draconic::Editor.Core Draconic::Editor.App Draconic::Policy)
target_sources(Draconic.Editor.Script PRIVATE ScriptPageImpl.cpp ScriptCompletionImpl.cpp ScriptApiSurfaceImpl.cpp ScriptApiBrowserImpl.cpp PUBLIC FILE_SET editor_script_modules TYPE CXX_MODULES FILES ScriptPage.cppm)

# The headless editing seam (ScriptSourceDocument, in draconic.script.editor) is covered by the
# script pipeline tests; the UI page itself is exercised by the editor smoke.

