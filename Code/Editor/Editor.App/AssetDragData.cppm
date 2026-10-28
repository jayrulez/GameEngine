// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :asset_drag_data partition
//
// What assets carry when they are dragged (Documentation/Specs/editor-gui.md, 5.2, layers 2 and 7):
// a GUI drag payload of type `asset/instance` holding, for each asset, its guid, its type's name
// (what a target takes it by) and its name (for the preview, and for a refusal's reason). A slot,
// a hierarchy or a page asks whether it takes one before the drop (Takes), naming why not when it
// does not; a group takes them all (ReadAll).
module;
#include "Core/Prelude.h"

export module editor.app:asset_drag_data;

import foundation.core;
import foundation.gui;

using namespace foundation::core;

export namespace editor::app
{
    /// AssetDrag::kType, at namespace scope for another library to read (MSVC exports no class's
    /// static constexpr member from a shared library).
    inline constexpr StringView kAssetDragType = u8"asset/instance";
    // A struct of static functions rather than functions: gcc 15 cannot read back a module whose
    // partition has an inline function at namespace scope that instantiates a template.
    struct AssetDrag
    {
        static constexpr StringView kType = kAssetDragType;

        Guid Id;
        String TypeName;
        String Name;

        /// The payload dragging `asset` carries.
        [[nodiscard]] static foundation::gui::DragPayload Payload(const Guid& id, StringView typeName, StringView name)
        {
            AssetDrag one{id, String(typeName), String(name)};
            return Payload(Span<const AssetDrag>(&one, 1u));
        }
        /// The payload dragging `assets` carries (a browser's selection), labelled by the one or
        /// by how many.
        [[nodiscard]] static foundation::gui::DragPayload Payload(Span<const AssetDrag> assets)
        {
            Array<Variant> parts;
            for (const AssetDrag& asset : assets)
            {
                utf8char guid[37];
                asset.Id.ToChars(guid);
                parts.PushBack(Variant::From<String>(String(StringView(guid, 36))));
                parts.PushBack(Variant::From<String>(asset.TypeName));
                parts.PushBack(Variant::From<String>(asset.Name));
            }
            foundation::gui::DragPayload payload;
            payload.Type = String(kType);
            payload.Value = Variant::From<Array<Variant>>(Move(parts));
            payload.Label = assets.Size() == 1 ? assets[0].Name : Format(u8"{} assets", assets.Size());
            payload.Allowed = foundation::gui::DropEffectsLink | foundation::gui::DropEffectsCopy | foundation::gui::DropEffectsMove;
            return payload;
        }

        /// Every asset a payload carries, in order; false when it carries none.
        [[nodiscard]] static bool ReadAll(const foundation::gui::DragPayload& payload, Array<AssetDrag>& out)
        {
            out.Clear();
            if (payload.Type.AsView() != kType)
                return false;
            const Array<Variant>* parts = payload.Value.TryGet<Array<Variant>>();
            if (parts == nullptr || parts->IsEmpty() || parts->Size() % 3 != 0)
                return false;
            for (usize i = 0; i < parts->Size(); i += 3)
            {
                const String* id = (*parts)[i].TryGet<String>();
                const String* type = (*parts)[i + 1].TryGet<String>();
                const String* name = (*parts)[i + 2].TryGet<String>();
                AssetDrag asset;
                if (id == nullptr || type == nullptr || name == nullptr || !Guid::TryParse(id->AsView(), asset.Id))
                {
                    out.Clear();
                    return false;
                }
                asset.TypeName = *type;
                asset.Name = *name;
                out.PushBack(Move(asset));
            }
            return true;
        }
        /// The first asset a payload carries; false when it carries none.
        [[nodiscard]] static bool Read(const foundation::gui::DragPayload& payload, AssetDrag& out)
        {
            Array<AssetDrag> all;
            if (!ReadAll(payload, all))
                return false;
            out = Move(all[0]);
            return true;
        }

        /// Whether a target that takes `types` (asset type names; empty: any) takes what `payload`
        /// carries; when not, `why` says so.
        [[nodiscard]] static bool Takes(const foundation::gui::DragPayload& payload, Span<const String> types, AssetDrag& out, String& why)
        {
            Array<AssetDrag> all;
            if (!ReadAll(payload, all))
            {
                why = String(u8"Not an asset");
                return false;
            }
            out = all[0];
            if (all.Size() > 1)
            {
                why = String(u8"This takes one asset");
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
