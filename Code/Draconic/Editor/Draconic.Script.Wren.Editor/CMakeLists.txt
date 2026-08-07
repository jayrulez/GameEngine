# Draconic::Script.Wren.Editor - `draconic.script.wren.editor` (the Wren cook service).
# The Wren-specific cook (compile-check + `static properties` Fiber probe + starter) that
# the neutral ScriptClassAssetBuilder resolves by language. Depends on the Wren backend
# (cook VM) + the neutral editor interface + facades (the cook VM's "main" surface).
add_library(Draconic.Script.Wren.Editor STATIC)
add_library(Draconic::Script.Wren.Editor ALIAS Draconic.Script.Wren.Editor)
target_link_libraries(Draconic.Script.Wren.Editor PUBLIC
    Draconic::Foundation Draconic::Editor Draconic::Editor.Core Draconic::Content
    Draconic::Script Draconic::Script.Resource Draconic::Script.Facades
    Draconic::Script.Editor Draconic::Script.Wren Draconic::Policy)
target_sources(Draconic.Script.Wren.Editor
    PRIVATE
        WrenScriptCookImpl.cpp
    PUBLIC
        FILE_SET script_wren_editor_modules TYPE CXX_MODULES FILES WrenScriptCook.cppm)

# Draconic::Script.Wren.Editor.UI - `draconic.script.wren.editor.ui`: Wren's EDITOR-UI services
# (CodeEditView lexer tables; the introspection completion provider joins later). Separate
# from the cook target above because that one links into Draconic.Tools.Cook/Draconic.Tools.Export, which must
# stay UI-free; only the editor app links this.
add_library(Draconic.Script.Wren.Editor.UI STATIC)
add_library(Draconic::Script.Wren.Editor.UI ALIAS Draconic.Script.Wren.Editor.UI)
target_link_libraries(Draconic.Script.Wren.Editor.UI PUBLIC
    Draconic::Foundation Draconic::UI.Toolkit Draconic::Policy)
target_sources(Draconic.Script.Wren.Editor.UI
    PRIVATE
        WrenEditorUIImpl.cpp
    PUBLIC
        FILE_SET script_wren_editor_ui_modules TYPE CXX_MODULES FILES WrenEditorUI.cppm)

