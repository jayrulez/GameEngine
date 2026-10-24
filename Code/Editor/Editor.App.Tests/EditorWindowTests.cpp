// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorWindow's look: the sheet a look makes from the editor's files, the
// density's sheet after the editor's so its sizes win, the user's accent as an input after both.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import editor.core;
import editor.app;

using namespace foundation::core;

namespace
{
    constexpr StringView kRoot = u8"editor-window"; // under the harness's .test-scratch

    void Write(StringView relative, StringView text)
    {
        const String full = PathJoin(kRoot, relative);
        (void)CreateDirectories(PathParent(full.AsView()));
        REQUIRE(WriteFile(full.AsView(), Span<const byte>(reinterpret_cast<const byte*>(text.Data()), text.Size())).IsOk());
    }
    usize Find(StringView text, StringView part)
    {
        for (usize i = 0; i + part.Size() <= text.Size(); ++i)
            if (text.SubStr(i, part.Size()) == part)
                return i;
        return text.Size();
    }
}

TEST_CASE("editor window: a look's sheet is the editor's, then its density's, then the user's accent")
{
    Write(u8"Editor/editor.css", u8".frame { color: var(--text); }");
    Write(u8"Editor/density-compact.css", u8":root { --row: 22px; }");
    Write(u8"Editor/density-comfortable.css", u8":root { --row: 32px; }");
    editor::EditorResources resources(DefaultAllocator(), kRoot);

    editor::app::EditorLook look;
    const String compact = editor::app::EditorWindow::SheetFor(resources, look);
    const usize editor = Find(compact.AsView(), u8".frame");
    const usize density = Find(compact.AsView(), u8"--row: 22px");
    const usize accent = Find(compact.AsView(), u8"--accent-input: #2f6fdf");
    CHECK(editor < density);
    CHECK(density < accent);
    CHECK(accent < compact.Size());

    look.density = String(u8"comfortable");
    look.accent = String(u8"#0f8a7e");
    const String comfortable = editor::app::EditorWindow::SheetFor(resources, look);
    CHECK(Find(comfortable.AsView(), u8"--row: 32px") < comfortable.Size());
    CHECK(Find(comfortable.AsView(), u8"--accent-input: #0f8a7e") < comfortable.Size());
}
