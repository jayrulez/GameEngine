// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::Script - the `editor.script` module.
//
// BuildScriptApiTree: the API browser's tree of the language's bound API (namespaces and classes,
// with their members under them) from the page's shared ScriptApiSurface, filtered by its search.

module;
#include "Core/Prelude.h"

module editor.script;

import foundation.core;
import foundation.script;

using namespace foundation::core;

namespace editor
{
    namespace script = foundation::script;

    namespace
    {
        [[nodiscard]] bool LessThan(StringView a, StringView b)
        {
            const usize common = a.Size() < b.Size() ? a.Size() : b.Size();
            for (usize i = 0; i < common; ++i)
            {
                if (a[i] != b[i])
                {
                    return a[i] < b[i];
                }
            }
            return a.Size() < b.Size();
        }

    }

    ScriptApiTree BuildScriptApiTree(const Array<script::ScriptApiType>& types,
                                     StringView filter)
    {
        // Alphabetical type order, sorted by index so the surface stays untouched.
        Array<usize> typeOrder;
        for (usize i = 0; i < types.Size(); ++i)
        {
            typeOrder.PushBack(i);
        }
        typeOrder.Sort(
            [&types](usize a, usize b)
            { return LessThan(types[a].scriptName.AsView(), types[b].scriptName.AsView()); });

        ScriptApiTree tree;
        for (usize typeIndex : typeOrder)
        {
            const script::ScriptApiType& type = types[typeIndex];
            const bool typeMatches = type.scriptName.AsView().ContainsIgnoreCase(filter);

            Array<usize> memberOrder;
            for (usize i = 0; i < type.members.Size(); ++i)
            {
                if (typeMatches ||
                    type.members[i].name.AsView().ContainsIgnoreCase(filter))
                {
                    memberOrder.PushBack(i);
                }
            }
            if (!typeMatches && memberOrder.IsEmpty())
            {
                continue; // neither the type nor any member survived the filter
            }
            memberOrder.Sort(
                [&type](usize a, usize b)
                { return LessThan(type.members[a].name.AsView(), type.members[b].name.AsView()); });

            ScriptApiTreeNode typeNode;
            typeNode.label = String(type.scriptName.AsView());
            if (IsEditorOnlyBinding(type.typeId))
            {
                typeNode.label.Append(u8" [editor]");
            }
            typeNode.insertText = String(type.scriptName.AsView());
            typeNode.depth = 0;
            const i32 typeNodeIndex = static_cast<i32>(tree.nodes.Size());
            tree.nodes.PushBack(Move(typeNode));
            tree.roots.PushBack(typeNodeIndex);

            for (usize memberIndex : memberOrder)
            {
                const script::ScriptApiMember& member = type.members[memberIndex];
                ScriptApiTreeNode memberNode;
                memberNode.label = String(member.signature.IsEmpty()
                                              ? member.name.AsView()
                                              : member.signature.AsView());
                memberNode.insertText = String(member.name.AsView());
                memberNode.depth = 1;
                const i32 memberNodeIndex = static_cast<i32>(tree.nodes.Size());
                tree.nodes.PushBack(Move(memberNode));
                tree.nodes[static_cast<usize>(typeNodeIndex)].children.PushBack(
                    memberNodeIndex);
            }
        }
        return tree;
    }
}
