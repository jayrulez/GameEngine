// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - the reactive layer's only per-thread state: the observer evaluating on this thread. It is
// defined in the library rather than in the interface, so every binary sees one slot per thread.

module;
#include "Core/Prelude.h"

module foundation.gui;

using namespace foundation::core;

namespace foundation::gui
{
    Observer*& CurrentObserverSlot() noexcept
    {
        thread_local Observer* current = nullptr;
        return current;
    }
}
