// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::Project - :allocator partition: the editor binary's root allocator.
module;
#include "Core/Prelude.h"

export module editor.project:allocator;

import foundation.core;

using namespace foundation::core;

export namespace editor
{
    namespace detail
    {
        // Not inline (defined in AllocatorImpl.cpp): a per-library slot would let the app install
        // the tagged root in its copy while other editor libraries keep allocating from the
        // untagged fallback, and a block allocated under one root and freed under another is a
        // cross-allocator free.
        [[nodiscard]] IAllocator*& EditorRootSlot() noexcept;
    }

    /// The editor binary's root allocator. The editor app installs its tagged "Editor" root at
    /// startup, so free helpers and panel providers (which have no owner parameter to thread)
    /// still attribute their allocations to the Editor tag. Outside the app (unit tests) it is
    /// the process allocator, since tests are their own roots.
    inline void SetEditorRootAllocator(IAllocator& allocator) noexcept
    {
        detail::EditorRootSlot() = &allocator;
    }
    [[nodiscard]] inline IAllocator& EditorRootAllocator() noexcept
    {
        return *detail::EditorRootSlot();
    }
}
