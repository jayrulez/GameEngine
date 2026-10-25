// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_picker_dialog partition
//
// The asset picker (Documentation/Specs/editor-gui.md, 5.2, layer 5): the picker over the
// project's assets of the types a reference takes (none named: any), each under its group
// ("Content" for the top), in the groups' name order, what it is beside its name, its type's
// glyph, and the favourites first. Its key is the asset's guid as text; None clears the reference.
// With none of those types it says so.
module;
#include "Core/Prelude.h"

export module editor.app:asset_picker_dialog;

import foundation.core;
import foundation.content;
import editor.core;
import :picker;

using namespace foundation::core;

export namespace editor::app
{
    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct AssetPicker
    {
        static constexpr StringView kTopGroup = u8"Content";

        /// The picker's request over `sources`' assets of `types` (asset type names; empty:
        /// any), `favourites` starred and first, `current` checked; the icons are `icons`'.
        [[nodiscard]] static PickerRequest Request(foundation::content::ContentDatabase& sources, editor::EditorIcons& icons, Span<const Guid> favourites,
                                                   Span<const String> types, const Guid& current)
        {
            PickerRequest request;
            request.Placeholder = String(u8"Search assets");
            request.AllowNone = true;
            if (!current.IsNil())
                request.Current = KeyOf(current);
            if (foundation::content::Group* top = sources.RootGroup())
                Collect(*top, icons, favourites, types, request.Items);
            String kinds;
            for (const String& type : types)
            {
                if (!kinds.IsEmpty())
                    kinds += u8" or ";
                kinds += type.AsView();
            }
            request.EmptyTitle = kinds.IsEmpty() ? String(u8"No assets yet") : Format(u8"No {} yet", kinds);
            request.EmptyText = String(u8"Import or create one in the Assets panel.");
            return request;
        }

        /// What a pick answers for an asset: its guid as text.
        [[nodiscard]] static String KeyOf(const Guid& id)
        {
            utf8char chars[37];
            id.ToChars(chars);
            return String(StringView(chars, 36));
        }

    private:
        static void Collect(foundation::content::Group& group, editor::EditorIcons& icons, Span<const Guid> favourites, Span<const String> types,
                            Array<PickerItem>& out)
        {
            const String section = group.Parent() == nullptr ? String(kTopGroup) : group.Path();
            for (foundation::content::Instance* instance : group.Instances())
            {
                if (!Takes(instance->TypeName(), types))
                    continue;
                PickerItem item;
                item.Key = KeyOf(instance->Id());
                item.Name = String(instance->Name());
                item.Detail = String(instance->TypeName());
                item.Section = section;
                for (const Guid& favourite : favourites)
                    item.Favourite = item.Favourite || favourite == instance->Id();
                const TypeInfo* type = GlobalTypeRegistry().FindByName(reinterpret_cast<const char*>(String(instance->TypeNamespace()).CStr()),
                                                                        reinterpret_cast<const char*>(String(instance->TypeName()).CStr()));
                if (type != nullptr)
                {
                    const editor::TypeIcon icon = icons.For(*type, u8"Asset");
                    item.Glyph = icon.Source;
                    item.GlyphClass = icon.Class;
                }
                out.PushBack(Move(item));
            }
            for (foundation::content::Group* child : group.Groups())
                Collect(*child, icons, favourites, types, out);
        }
        [[nodiscard]] static bool Takes(StringView typeName, Span<const String> types)
        {
            if (types.IsEmpty())
                return true;
            for (const String& type : types)
                if (type.AsView() == typeName)
                    return true;
            return false;
        }
    };
}
