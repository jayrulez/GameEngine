# Draconic::Script.AngelScript.Editor - `draconic.script.angelscript.editor` (the AngelScript
# cook service): compile-check + shared handler scan + [metadata] PROPERTY HARVEST + starter.
# Depends on the AngelScript backend (cook VM + engine-handle seam) + the neutral editor
# interface + facades (the cook VM's "main" surface). The SDK (Draconic::AngelScript) is a
# PRIVATE dep: the cook's implementation unit #includes <angelscript.h> and the vendored
# <scriptbuilder/scriptbuilder.h> to drive metadata harvest (GCC module hygiene - impl only).
add_library(Draconic.Script.AngelScript.Editor STATIC)
add_library(Draconic::Script.AngelScript.Editor ALIAS Draconic.Script.AngelScript.Editor)
target_link_libraries(Draconic.Script.AngelScript.Editor
    PUBLIC
        Draconic::Foundation Draconic::Editor Draconic::Editor.Core Draconic::Content
        Draconic::Script Draconic::Script.Resource Draconic::Script.Facades
        Draconic::Script.Editor Draconic::Script.AngelScript Draconic::Policy
    PRIVATE
        Draconic::AngelScript)
target_sources(Draconic.Script.AngelScript.Editor
    PRIVATE
        AngelScriptScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_as_editor_modules TYPE CXX_MODULES FILES AngelScriptScriptCook.cppm)

# Draconic::Script.AngelScript.Editor.UI - `draconic.script.angelscript.editor.ui`: the
# EDITOR-UI services (CodeEditView lexer tables; the introspection completion provider joins
# later). Separate from the cook target above because that one links into Draconic.Tools.Cook/
# Draconic.Tools.Export, which must stay UI-free; only the editor app links this.
add_library(Draconic.Script.AngelScript.Editor.UI STATIC)
add_library(Draconic::Script.AngelScript.Editor.UI ALIAS Draconic.Script.AngelScript.Editor.UI)
target_link_libraries(Draconic.Script.AngelScript.Editor.UI PUBLIC
    Draconic::Foundation Draconic::UI.Toolkit Draconic::Policy)
target_sources(Draconic.Script.AngelScript.Editor.UI
    PRIVATE
        AngelScriptEditorUIImpl.cpp
    PUBLIC
        FILE_SET script_as_editor_ui_modules TYPE CXX_MODULES FILES AngelScriptEditorUI.cppm)

