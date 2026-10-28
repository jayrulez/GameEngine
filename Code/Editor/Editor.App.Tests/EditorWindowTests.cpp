// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Editor::App - EditorWindow's look: the sheet a look makes from the editor's files, the
// density's sheet after the editor's so its sizes win, the user's accent as an input after both;
// a notice's toast, staying with its button when it has an action.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
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

TEST_CASE("editor window: the look from the user's settings, the defaults for what is unset or named wrongly, and back")
{
    const editor::app::EditorLook none = editor::app::LookFrom(nullptr);
    CHECK(none.dark);
    CHECK(none.accent == String(u8"#2f6fdf"));
    CHECK(none.density == String(u8"compact"));
    CHECK_FALSE(none.reduceMotion);

    editor::EditorUiSettings ui;
    ui.scheme = String(u8"light");
    ui.accent = String(u8"#0f8a7e");
    ui.density = String(u8"comfortable");
    ui.reduceMotion = true;
    ui.uiScale = 9.0f; // past the range
    editor::app::EditorLook look = editor::app::LookFrom(&ui);
    CHECK_FALSE(look.dark);
    CHECK(look.accent == String(u8"#0f8a7e"));
    CHECK(look.density == String(u8"comfortable"));
    CHECK(look.reduceMotion);
    CHECK(look.uiScale == doctest::Approx(editor::kUiScaleMax));

    ui.accent = String(u8"red"); // no colour
    ui.density = String(u8"roomy"); // no sheet
    look = editor::app::LookFrom(&ui);
    CHECK(look.accent == String(u8"#2f6fdf"));
    CHECK(look.density == String(u8"compact"));

    editor::EditorUiSettings stored;
    stored.autoOrthographic = false;
    editor::app::EditorLook chosen;
    chosen.dark = false;
    chosen.accent = String(u8"#6d5bd8");
    chosen.density = String(u8"default");
    chosen.reduceMotion = true;
    chosen.uiScale = 1.25f;
    editor::app::StoreLook(stored, chosen);
    CHECK(stored.scheme == String(u8"light"));
    CHECK(stored.accent == String(u8"#6d5bd8"));
    CHECK(stored.density == String(u8"default"));
    CHECK(stored.reduceMotion);
    CHECK(stored.uiScale == doctest::Approx(1.25f));
    CHECK_FALSE(stored.autoOrthographic); // kept
}

TEST_CASE("editor window: a notice's toast times out; one with an action (Open Folder) stays with its button")
{
    using editor::app::EditorWindow;
    const foundation::gui::ToastRequest plain = EditorWindow::MakeToast(editor::NoticeKind::Success, u8"Cook finished");
    CHECK(plain.Severity == foundation::gui::ToastSeverity::Success);
    CHECK(plain.Title == String(u8"Cook finished"));
    CHECK(plain.Duration < 0.0f); // the severity's default
    CHECK(plain.Actions.IsEmpty());

    const foundation::gui::ToastRequest exported = EditorWindow::MakeToast(editor::NoticeKind::Success, u8"Exported to Dist/Linux64", u8"Open Folder");
    CHECK(exported.Duration == 0.0f); // until closed
    REQUIRE(exported.Actions.Size() == 1u);
    CHECK(exported.Actions[0].Text == String(u8"Open Folder"));
}
