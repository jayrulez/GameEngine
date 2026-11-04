// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// nativesample.pipeline - the reference game's own asset type: a note, a line of text the game
// authors in the editor, cooked into its product by its builder.

module;
#include "Core/Prelude.h"
#include "Core/Reflection/Reflect.h"

export module nativesample.pipeline;

import foundation.core;
import foundation.content;
import pipeline.core;

export namespace nativesample
{
    // A note as the editor authors it.
    class NoteAsset final : public pipeline::Asset
    {
        RTTI_OBJECT(NoteAsset, pipeline::Asset)
    public:
        foundation::core::String text = foundation::core::String(u8"A note from the game's own code");

        void Serialize(foundation::core::ISerializer& ar) override
        {
            pipeline::Asset::Serialize(ar);
            foundation::core::Serialize(ar, "text", text);
        }
    };

    // A note as the game reads it, cooked.
    class NoteProduct final : public foundation::core::ISerializable
    {
        RTTI_OBJECT(NoteProduct, foundation::core::ISerializable)
    public:
        foundation::core::String text;

        void Serialize(foundation::core::ISerializer& ar) override { foundation::core::Serialize(ar, "text", text); }
    };

    // Cooks a note: its text, as written.
    class NoteBuilder final : public pipeline::DefaultAssetBuilder
    {
    public:
        [[nodiscard]] const foundation::core::TypeInfo* AssetType() const override { return &NoteAsset::StaticType(); }
        [[nodiscard]] const foundation::core::TypeInfo* ProductType() const override { return &NoteProduct::StaticType(); }
        [[nodiscard]] foundation::core::u32 Version() const override { return 1; }
        [[nodiscard]] foundation::core::Status Build(const pipeline::Asset& asset, pipeline::AssetBuildContext& ctx) override
        {
            NoteProduct product;
            product.text = static_cast<const NoteAsset&>(asset).text;
            return ctx.output->WriteObject(product);
        }
    };

    // Visits every note under `group`, its subgroups included.
    template <typename Visit>
    void ForEachNote(const foundation::content::Group& group, const Visit& visit)
    {
        for (foundation::content::Instance* instance : group.Instances())
        {
            if (instance->TypeName() == foundation::core::StringView(u8"NoteAsset"))
            {
                visit(*instance);
            }
        }
        for (foundation::content::Group* child : group.Groups())
        {
            ForEachNote(*child, visit);
        }
    }

    // The creator's label, as File > New and asset_create name it.
    inline constexpr foundation::core::StringView kNoteCreator = u8"Native Sample Note";
}
