// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_drag_data partition
//
// What an asset carries when it is dragged (Documentation/Specs/editor-gui.md, 5.2, layer 2): a GUI
// drag payload of type `asset/instance` holding the asset's guid, its type's name (what a target
// takes it by) and its name (for the preview, and for a refusal's reason). A slot, a hierarchy or a
// page asks whether it takes the asset before the drop (Takes), naming why not when it does not.
module;
#include "Core/Prelude.h"

export module editor.app:asset_drag_data;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor::app
{
    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct AssetDrag
    {
        static constexpr StringView kType = u8"asset/instance";

        Guid Id;
        String TypeName;
        String Name;

        /// The payload dragging `asset` carries.
        [[nodiscard]] static foundation::gui::DragPayload Payload(const Guid& id, StringView typeName, StringView name)
        {
            utf8char guid[37];
            id.ToChars(guid);
            Array<Variant> parts;
            parts.PushBack(Variant::From<String>(String(StringView(guid, 36))));
            parts.PushBack(Variant::From<String>(String(typeName)));
            parts.PushBack(Variant::From<String>(String(name)));
            foundation::gui::DragPayload payload;
            payload.Type = String(kType);
            payload.Value = Variant::From<Array<Variant>>(Move(parts));
            payload.Label = String(name);
            payload.Allowed = foundation::gui::DropEffectsLink | foundation::gui::DropEffectsCopy;
            return payload;
        }

        /// The asset a payload carries; false when it carries none.
        [[nodiscard]] static bool Read(const foundation::gui::DragPayload& payload, AssetDrag& out)
        {
            if (payload.Type.AsView() != kType)
                return false;
            const Array<Variant>* parts = payload.Value.TryGet<Array<Variant>>();
            if (parts == nullptr || parts->Size() != 3)
                return false;
            const String* id = (*parts)[0].TryGet<String>();
            const String* type = (*parts)[1].TryGet<String>();
            const String* name = (*parts)[2].TryGet<String>();
            if (id == nullptr || type == nullptr || name == nullptr || !Guid::TryParse(id->AsView(), out.Id))
                return false;
            out.TypeName = *type;
            out.Name = *name;
            return true;
        }

        /// Whether a target that takes `types` (asset type names; empty: any) takes what `payload`
        /// carries; when not, `why` says so.
        [[nodiscard]] static bool Takes(const foundation::gui::DragPayload& payload, Span<const String> types, AssetDrag& out, String& why)
        {
            if (!Read(payload, out))
            {
                why = String(u8"Not an asset");
                return false;
            }
            if (types.IsEmpty())
                return true;
            for (const String& type : types)
                if (type == out.TypeName)
                    return true;
            why = Format(u8"{} is a {}, not one this takes", out.Name, out.TypeName);
            return false;
        }
    };
}
