// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :model_index partition
//
// ModelIndex and ModelRole: the address of a cell in a Model, and the aspect a view asks for. An
// index is a row and a column, and for a tree model the node's opaque id (InternalId) that locates
// it under its parent.

module;
#include "Core/Prelude.h"

export module foundation.gui:model_index;

import foundation.core;

using namespace foundation::core;

export namespace foundation::gui
{
    // Which aspect of a cell a view is requesting (display text, a sort key, an icon, ...).
    enum class ModelRole
    {
        Display,
        Sort,
        Icon,      ///< an image's source (a path), or nothing
        Custom,
        IconClass, ///< a class on the icon image for a sheet to style (a type's category tint)
        RowClass,  ///< space-separated classes on the row's content for a sheet to style (a state)
    };

    struct ModelIndex
    {
        i32 Row = -1; // -1: invalid
        i32 Column = 0;
        i64 InternalId = -1; // opaque node identity a tree model uses to locate the node

        [[nodiscard]] bool IsValid() const noexcept { return Row >= 0 && Column >= 0; }
        [[nodiscard]] bool operator==(const ModelIndex& other) const noexcept
        {
            return Row == other.Row && Column == other.Column && InternalId == other.InternalId;
        }
    };

    [[nodiscard]] inline ModelIndex MakeModelIndex(i32 row, i32 column = 0,
                                                   i64 internalId = -1) noexcept
    {
        return ModelIndex{row, column, internalId};
    }
}
