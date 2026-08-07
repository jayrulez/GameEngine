// Draconic::ScriptWrenEditorUI - the `draconic.script.wren.editor.ui` module.
//
// Wren-specific EDITOR-UI services: everything the in-editor experience needs that depends on
// ui.toolkit and therefore cannot live in the cook target (Draconic::ScriptWrenEditor links
// into Draconic.Tools.Cook/Draconic.Tools.Export, which must stay UI-free). Today that is the Wren syntax
// tables for CodeEditView highlighting, registered into the toolkit's CodeLexerRegistry by
// language id; the Wren completion provider (ICompletionProvider over the introspection
// battery) joins here with code-editor P4.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.script.wren.editor.ui;

import draconic.foundation;

export namespace draconic::script::wren
{
    /// Registers Wren's editor-UI services (the CodeEditView lexer). An editor entry point's
    /// job, beside RegisterWrenScriptBackend/RegisterWrenScriptCook.
    void RegisterWrenEditorUI();
}
