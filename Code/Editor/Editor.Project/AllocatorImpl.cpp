// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// The single slot for the editor binary's root allocator (declared in Allocator.cppm): one per
// process, so every editor library allocates from the same root the app installed.
module;
#include "Core/Prelude.h"

module editor.project;

import foundation.core;

using namespace foundation::core;

namespace editor::detail
{
    IAllocator*& EditorRootSlot() noexcept
    {
        static IAllocator* slot = &DefaultAllocator();
        return slot;
    }
} // namespace editor::detail
