// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell
// Editor::App tests: the problems list. Errors come first, each kind ordered by position, with a
// line saying how many there are. A row chooses its problem, and a problem in an include is named
// by its file.
#include <doctest/doctest.h>
#include "Core/Prelude.h"

import foundation.core;
import foundation.gui;
import foundation.gui.code;
import editor.app;

using namespace foundation::core;
namespace gui = foundation::gui;
using editor::app::CodeProblem;

TEST_CASE("problems list: errors first in their places' order, the count said, a row chosen goes to its problem")
{
    RefPtr<gui::SceneRoot> root = MakeRef<gui::SceneRoot>(DefaultAllocator(), MakeRef<gui::GUIContext>(DefaultAllocator()));
    RefPtr<gui::Panel> host = MakeRef<gui::Panel>(DefaultAllocator());
    root->AddChild(host.Get());
    editor::app::ProblemsList problems(DefaultAllocator(), *host);
    CHECK(problems.Summary() == StringView(u8"No problems"));
    problems.SetClean(String(u8"No problems: compiles class Hero")); // what the page's check found good
    CHECK(problems.Summary() == StringView(u8"No problems: compiles class Hero"));
    problems.SetClean(String{});

    const CodeProblem found[] = {
        CodeProblem{gui::CodeSeverity::Warning, String(u8"unused variable"), String{}, 2, 4, true},
        CodeProblem{gui::CodeSeverity::Error, String(u8"no such type"), String{}, 9, 0, true},
        CodeProblem{gui::CodeSeverity::Error, String(u8"in the include"), String(u8"Shaders/Common.hlsli"), 0, 0, true},
        CodeProblem{gui::CodeSeverity::Error, String(u8"missing ;"), String{}, 3, 7, true},
    };
    problems.Show(Span<const CodeProblem>(found, 4));
    CHECK(problems.Summary() == StringView(u8"3 errors, 1 warning"));
    const Span<const CodeProblem> shown = problems.Problems();
    REQUIRE(shown.Size() == 4u);
    CHECK(shown[0].message == StringView(u8"missing ;"));   // errors in its own code, by line
    CHECK(shown[1].message == StringView(u8"no such type"));
    CHECK(shown[2].message == StringView(u8"in the include")); // then those in an include
    CHECK(shown[3].severity == gui::CodeSeverity::Warning);
    CHECK(editor::app::ProblemsList::PlaceOf(shown[0]) == StringView(u8"4:8"));
    CHECK(editor::app::ProblemsList::PlaceOf(shown[2]) == StringView(u8"Common.hlsli 1:1"));
    CodeProblem lineOnly = shown[0];
    lineOnly.atColumn = false; // the check named the line alone
    CHECK(editor::app::ProblemsList::PlaceOf(lineOnly) == StringView(u8"4"));

    String chosen;
    problems.SetOnChoose([&chosen](const CodeProblem& p) { chosen = p.message; });
    problems.Choose(gui::ModelIndex{1, 0});
    CHECK(chosen == StringView(u8"no such type"));
    problems.Choose(gui::ModelIndex{9, 0}); // no such row: nothing
    CHECK(chosen == StringView(u8"no such type"));

    // A code editor's diagnostics are its own code's problems.
    gui::CodeDiagnostic d;
    d.Range.Start = gui::CodePosition{5, 2};
    d.Severity = gui::CodeSeverity::Warning;
    d.Message = String(u8"shadowed");
    const Array<CodeProblem> fromEditor = editor::app::ProblemsOf(Span<const gui::CodeDiagnostic>(&d, 1));
    REQUIRE(fromEditor.Size() == 1u);
    CHECK(fromEditor[0].line == 5u);
    CHECK(fromEditor[0].file.IsEmpty());
}
