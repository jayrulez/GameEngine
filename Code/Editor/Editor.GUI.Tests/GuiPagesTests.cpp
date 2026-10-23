// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// The GUI asset pages' headless halves (E10 of Specs/gui-lab-evolution-3.md): what the cook reports
// marks the editor's lines, and the markup a sheet is first previewed over builds clean.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.ui;
import foundation.ui.toolkit;
import experimental.gui;
import experimental.gui.pipeline;
import editor.gui;

using namespace foundation::core;
namespace toolkit = foundation::ui::toolkit;
namespace lab = experimental::gui;

TEST_CASE("gui editor: what the cook reports marks the editor's lines, and clears when clean")
{
    RefPtr<toolkit::CodeEditView> view = MakeRef<toolkit::CodeEditView>(DefaultAllocator());
    toolkit::CodeEditView& editor = *view;
    const StringView broken = u8"<Panel>\n  <NoSuchElement/>\n</Panel>";
    editor.SetText(broken);
    Array<lab::GuiDiagnostic> found;
    (void)pipeline::ValidateGuiDocument(broken, found, DefaultAllocator());
    REQUIRE_FALSE(found.IsEmpty()); // an element nothing makes is dropped, and said so
    editor::gui_pages::MarkLines(editor, found);
    REQUIRE(editor.Document().Diagnostics().Size() == found.Size());
    // The editor counts from 0; a diagnostic with no line (0) marks the first.
    const i32 expected = found[0].Line > 0 ? static_cast<i32>(found[0].Line) - 1 : 0;
    CHECK(editor.Document().Diagnostics()[0].line == expected);
    CHECK_FALSE(editor.Document().Diagnostics()[0].message.IsEmpty());

    Array<lab::GuiDiagnostic> none;
    editor::gui_pages::MarkLines(editor, none);
    CHECK(editor.Document().Diagnostics().IsEmpty());
}

TEST_CASE("gui editor: the markup a sheet is first previewed over builds clean")
{
    Array<lab::GuiDiagnostic> found;
    CHECK(pipeline::ValidateGuiDocument(editor::gui_pages::kStockPreviewMarkup, found, DefaultAllocator()));
    CHECK(found.IsEmpty());
}
