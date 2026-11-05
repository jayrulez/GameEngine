// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor App - :about_box partition
//
// Help > About: the engine's name and this build's identity (the version with its label, the build
// number CI stamped or "local build", the git stamp), and a Copy Version button beside OK that hands
// the same line to the caller to put on the clipboard, so a report names the exact build.

module;
#include "Core/Prelude.h"

export module editor.app:about_box;

import foundation.core;
import foundation.gui;
import engine.project; // kEngineProductName

using namespace foundation::core;

export namespace editor::app
{
    /// The About box's text and what Copy Version copies: the engine's name and `identity`.
    [[nodiscard]] inline String AboutLine(StringView identity)
    {
        return Format(u8"{} {}", engine::project::kEngineProductName, identity);
    }

    /// The About box over `identity` (engine::project::BuildIdentity): OK, and Copy Version, which
    /// calls `copy` with the line before the box closes.
    [[nodiscard]] inline RefPtr<foundation::gui::MessageBox> MakeAboutBox(IAllocator& allocator, StringView identity,
                                                                          Function<void(StringView)> copy)
    {
        RefPtr<foundation::gui::MessageBox> about = MakeRef<foundation::gui::MessageBox>(allocator);
        const String line = AboutLine(identity);
        about->Configure(Format(u8"About {}", engine::project::kEngineProductName).AsView(), line.AsView(),
                         foundation::gui::MessageBox::Buttons::Ok);
        (void)about->AddButton(u8"Copy Version", u8"copy");
        foundation::gui::MessageBox* box = about.Get(); // the listener is the box's own
        (void)about->AddEventListener(foundation::gui::EventType::Changed,
                                      [box, copy = Move(copy), line](const foundation::gui::Event&)
                                      {
                                          if (copy && box->GetResultName() == StringView(u8"copy"))
                                              copy(line.AsView());
                                      });
        return about;
    }
}
