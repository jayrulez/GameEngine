// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :group_picker_dialog partition
//
// The group picker (Documentation/Specs/editor-gui.md, 5.2, layer 5): the picker over the
// project's groups, where an import or a new asset goes. The groups are a tree: each a row by its
// name, indented under what holds it, in the tree's order (a group, then what is under it), the top
// one "Content"; the current group is checked and lit, and the search finds a group by its name,
// keeping the groups above it so two of one name read apart. Its key is the group's place: "Content", or "Content/" and its path.
module;
#include "Core/Prelude.h"

export module editor.app:group_picker_dialog;

import foundation.core;
import foundation.content;
import :picker;

using namespace foundation::core;

export namespace editor::app
{
    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct GroupPicker
    {
        static constexpr StringView kTop = u8"Content";

        /// The picker's request over `sources`' groups, `current` checked.
        [[nodiscard]] static PickerRequest Request(foundation::content::ContentDatabase& sources, const foundation::content::Group* current)
        {
            PickerRequest request;
            request.Placeholder = String(u8"Search groups");
            request.Tree = true;
            if (current != nullptr)
                request.Current = KeyOf(*current);
            if (foundation::content::Group* top = sources.RootGroup())
                Collect(*top, 0, request.Items);
            return request;
        }

        /// A group's key: where it is, from the top.
        [[nodiscard]] static String KeyOf(const foundation::content::Group& group)
        {
            return group.Parent() == nullptr ? String(kTop) : Format(u8"{}/{}", kTop, group.Path());
        }
        /// How a group is named to the user: "Content", or its path.
        [[nodiscard]] static String ShownName(const foundation::content::Group& group)
        {
            return group.Parent() == nullptr ? String(kTop) : group.Path();
        }
        /// The group a key names; null when there is none.
        [[nodiscard]] static foundation::content::Group* Find(foundation::content::ContentDatabase& sources, StringView key)
        {
            foundation::content::Group* group = sources.RootGroup();
            if (group == nullptr || !key.StartsWith(kTop))
                return nullptr;
            usize at = kTop.Size();
            while (group != nullptr && at < key.Size())
            {
                if (key[at] != u8'/')
                    return nullptr;
                usize end = at + 1;
                while (end < key.Size() && key[end] != u8'/')
                    ++end;
                group = group->GetGroup(key.SubStr(at + 1, end - at - 1));
                at = end;
            }
            return group;
        }

    private:
        static void Collect(foundation::content::Group& group, u32 depth, Array<PickerItem>& out)
        {
            PickerItem item;
            item.Key = KeyOf(group);
            item.Name = group.Parent() == nullptr ? String(kTop) : String(group.Name());
            item.Glyph = String(u8"Editor/Icons/folder.svg");
            item.Depth = depth;
            out.PushBack(Move(item));
            for (foundation::content::Group* child : group.Groups())
                Collect(*child, depth + 1, out);
        }
    };
}
