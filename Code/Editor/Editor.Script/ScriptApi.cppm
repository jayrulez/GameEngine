// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - :script_api partition.
//
// What a script may call, as the runtime binds it for one language (IScriptManager::
// DescribeBoundApi): built once per page and read by its completion, its hover and its API browser
// alike, so they cannot disagree. The browser's tree is a pure function of it (BuildScriptApiTree).

module;
#include "Core/Prelude.h"

export module editor.script:script_api;

import foundation.core;
import foundation.script;

using namespace foundation::core;

export namespace editor
{
    // The backend's ACTUAL bound API for one language, built once and shared by every consumer.
    // The build replays the runtime's exact registration sequence against a throwaway manager
    // (RegisterCoreTypes + facade reflection + CreateScriptManagerForLanguage +
    // RegisterReflectedTypes), so what the page shows is exactly what a run can call, spelled the
    // language's way. Lazy: nothing runs until the first Types() call; an unknown language stays
    // empty.
    class ScriptApiSurface final
    {
    public:
        void SetLanguage(StringView languageId) { m_language = String(languageId); }
        [[nodiscard]] const Array<foundation::script::ScriptApiType>& Types() const;
        [[nodiscard]] const foundation::script::ScriptApiType* FindType(StringView name) const
        {
            for (const foundation::script::ScriptApiType& type : Types())
                if (type.scriptName.AsView() == name)
                    return &type;
            return nullptr;
        }

    private:
        String m_language;
        mutable bool m_built = false;
        mutable Array<foundation::script::ScriptApiType> m_types;
    };

    // True when a binding's reflected type was registered under a non-Runtime domain
    // (TypeRegistry::DomainOf): callable wherever the editor runs (including play-in-editor), but
    // ABSENT from a built player. Completion and the API browser both mark such bindings
    // " [editor]" so a player-bound script's author is warned.
    [[nodiscard]] inline bool IsEditorOnlyBinding(TypeId typeId)
    {
        return typeId != 0 && !(GlobalTypeRegistry().DomainOf(typeId) == kRuntimeTypeDomain);
    }

    // One row of the API browser tree: a type/namespace (depth 0) or a member (depth 1).
    struct ScriptApiTreeNode
    {
        String label;      // row text: type name, or the member's language-formatted signature
        String insertText; // what choosing the row types into the code
        i32 depth = 0;
        Array<i32> children; // indices into ScriptApiTree::nodes (type rows only)
    };

    struct ScriptApiTree
    {
        Array<ScriptApiTreeNode> nodes;
        Array<i32> roots; // depth-0 node indices, display order
    };

    // Builds the browser tree from a bound-API surface: types alphabetical, each type's members
    // alphabetical under it. `filter` is a case-insensitive substring match: a matching TYPE name
    // keeps the whole type; otherwise only matching members (and their type row) survive. Empty
    // filter keeps everything.
    [[nodiscard]] ScriptApiTree BuildScriptApiTree(const Array<foundation::script::ScriptApiType>& types, StringView filter);
}
