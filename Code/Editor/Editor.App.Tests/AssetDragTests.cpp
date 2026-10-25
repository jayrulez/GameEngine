// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// What an asset carries when dragged: the payload it makes, read back whole; a target that takes
// some types taking it or naming why not; anything else not an asset.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import editor.app;

using namespace foundation::core;
using editor::app::AssetDrag;
namespace gui = foundation::gui;

TEST_CASE("asset drag: the payload carries the asset's guid, type and name, and a target takes it by type")
{
    const Guid id{0xabcdu, 0x1234u};
    const gui::DragPayload payload = AssetDrag::Payload(id, u8"MaterialAsset", u8"Wood");
    CHECK(payload.Type == String(AssetDrag::kType));
    CHECK(payload.Label == String(u8"Wood"));

    AssetDrag read;
    REQUIRE(AssetDrag::Read(payload, read));
    CHECK(read.Id == id);
    CHECK(read.TypeName == String(u8"MaterialAsset"));
    CHECK(read.Name == String(u8"Wood"));

    String why;
    const String materials[] = {String(u8"MaterialAsset")};
    CHECK(AssetDrag::Takes(payload, Span<const String>(materials, 1), read, why));
    CHECK(AssetDrag::Takes(payload, Span<const String>{}, read, why)); // a target that takes any
    const String meshes[] = {String(u8"StaticMeshAsset"), String(u8"SkinnedMeshAsset")};
    CHECK_FALSE(AssetDrag::Takes(payload, Span<const String>(meshes, 2), read, why));
    CHECK(why == String(u8"Wood is a MaterialAsset, not one this takes"));

    gui::DragPayload text;
    text.Type = String(u8"text");
    text.Value = Variant::From<String>(String(u8"hello"));
    CHECK_FALSE(AssetDrag::Read(text, read));
    CHECK_FALSE(AssetDrag::Takes(text, Span<const String>{}, read, why));
    CHECK(why == String(u8"Not an asset"));
}
