// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Help > About: the box shows the engine's name (from CMake) and the build's identity, and Copy Version hands
// that line to the clipboard's caller; OK copies nothing.

#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import engine.project; // kEngineProductName
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;

TEST_CASE("about box: Copy Version hands over the engine's name and the build's identity; OK copies nothing")
{
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    root->SetSize(Float2{900.0f, 700.0f});
    constexpr StringView kIdentity = u8"0.1.0-beta (build 412, abc1234 @ 2026-11-04 15:03)";
    String copied;
    int copies = 0;
    const auto copy = [&](StringView line)
    {
        copied = String(line);
        ++copies;
    };
    CHECK(editor::app::AboutLine(kIdentity) == Format(u8"{} 0.1.0-beta (build 412, abc1234 @ 2026-11-04 15:03)",
                                                      engine::project::kEngineProductName));

    RefPtr<gui::MessageBox> about = editor::app::MakeAboutBox(DefaultAllocator(), kIdentity, copy);
    about->OpenModal(*root);
    REQUIRE(about->ButtonCount() == 2u); // OK, then Copy Version
    CHECK(about->ButtonAt(1)->HandleActivate());
    CHECK(copies == 1);
    CHECK(copied == editor::app::AboutLine(kIdentity));
    CHECK_FALSE(about->IsOpen());

    RefPtr<gui::MessageBox> again = editor::app::MakeAboutBox(DefaultAllocator(), kIdentity, copy);
    again->OpenModal(*root);
    CHECK(again->ButtonAt(0)->HandleActivate());
    CHECK(copies == 1);
}
