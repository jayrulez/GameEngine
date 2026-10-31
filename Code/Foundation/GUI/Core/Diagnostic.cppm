// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI - :diagnostic partition
//
// A message from a loader or parser about its input: a line (1-based; 0 when it is not known) and
// the message text. The markup loader and the CSS parser fill a list of these when given one, so a
// cook can warn about what they dropped or fail on what they could not read. Without a list they
// log instead, which is what a running GUI does.

module;
#include "Core/Prelude.h"

export module foundation.gui:diagnostic;

import foundation.core;

using namespace foundation::core;
namespace core = foundation::core;

export namespace foundation::gui
{
    struct GuiDiagnostic
    {
        enum class Severity : u8
        {
            Warning, ///< something was dropped; the rest is kept
            Error,   ///< the input could not be read
        };
        Severity Level = Severity::Warning;
        u32 Line = 0; ///< 1-based; 0 when not known
        core::String Message;
    };
}
