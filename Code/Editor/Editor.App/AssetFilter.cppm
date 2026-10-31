// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor App - :asset_filter partition
//
// What the asset browser's and the asset picker's filter fields match: an asset by its name or its
// guid, as every editor filter field matches (editor.core :search_filter, NameOrGuidMatches; a
// whole guid, FilterAsGuid, selects it).
module;
#include "Core/Prelude.h"

export module editor.app:asset_filter;

import foundation.core;
import foundation.content;
import editor.core;

using namespace foundation::core;

export namespace editor::app
{
    /// Whether the filter field's text matches `instance`: its name contains the text (any case),
    /// or its guid begins with the text's hex digits. Empty text matches everything.
    [[nodiscard]] inline bool AssetMatchesFilter(const foundation::content::Instance& instance, StringView filter)
    {
        return NameOrGuidMatches(instance.Name(), instance.Id(), filter);
    }
}
