// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

/// Defines a portion of a mesh that uses a specific material.

export module foundation.model:mesh_part;

import foundation.core;

using namespace foundation::core;

export namespace foundation::model
{

    /// A sub-range of a mesh's index buffer bound to one material.
    struct ModelMeshPart
    {
        i32 indexStart = 0;     // Starting index in the index buffer.
        i32 indexCount = 0;     // Number of indices in this part.
        i32 materialIndex = -1; // Material index (-1 for no material).

        constexpr ModelMeshPart() = default;
        constexpr ModelMeshPart(i32 start, i32 count, i32 matIdx = -1)
            : indexStart(start), indexCount(count), materialIndex(matIdx)
        {
        }
    };

} // namespace foundation::model
