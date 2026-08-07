// Draconic::EditorScript - the `draconic.editor.script` module.
//
// ScriptApiCompletionProvider implementation: serves candidates from the page's SHARED
// ScriptApiSurface (the same data the API browser shows) - type/namespace names at top
// level, a type's members after `Type.`.

module;
#include "Draconic.Foundation/Prelude.h"

module draconic.editor.script;

import draconic.foundation;
import draconic.ui;
import draconic.ui.toolkit;
import draconic.script;

using namespace draconic::foundation;

namespace draconic::editor
{
    namespace toolkit = draconic::ui::toolkit;
    namespace script = draconic::script;

    void ScriptApiCompletionProvider::Collect(const toolkit::CodeDocument& document,
                                              toolkit::CodePosition cursor, StringView prefix,
                                              Array<toolkit::CompletionCandidate>& out)
    {
        if (m_surface == nullptr)
        {
            return;
        }
        const Array<script::ScriptApiType>& types = m_surface->Types();
        if (types.IsEmpty())
        {
            return;
        }

        // Member context: the prefix sits immediately right of a '.', and the word before
        // that dot names a bound type/namespace.
        const i32 anchorColumn = cursor.column - static_cast<i32>(Utf8Length(prefix));
        if (anchorColumn >= 1 &&
            document.CodepointAt(toolkit::CodePosition{cursor.line, anchorColumn - 1}) == u8'.')
        {
            if (anchorColumn < 2)
            {
                return;
            }
            const toolkit::CodeSpan owner =
                document.WordAt(toolkit::CodePosition{cursor.line, anchorColumn - 2});
            if (owner.IsEmpty())
            {
                return;
            }
            const String ownerName = document.TextInSpan(owner);
            for (const script::ScriptApiType& type : types)
            {
                if (type.scriptName.AsView() != ownerName.AsView())
                {
                    continue;
                }
                for (const script::ScriptApiMember& member : type.members)
                {
                    out.PushBack(toolkit::CompletionCandidate{String(member.name.AsView()),
                                                              String(member.name.AsView())});
                }
                return;
            }
            return; // unknown receiver: offer nothing (the word provider still contributes)
        }

        // Top level: the bound type/namespace names. Editor-only bindings carry a label
        // marker (insert text stays the bare name) - they work in-editor/PIE but are
        // absent from a shipped player.
        for (const script::ScriptApiType& type : types)
        {
            String label(type.scriptName.AsView());
            if (IsEditorOnlyBinding(type.typeId))
            {
                label.Append(u8" [editor]");
            }
            out.PushBack(toolkit::CompletionCandidate{Move(label),
                                                      String(type.scriptName.AsView())});
        }
    }
}
