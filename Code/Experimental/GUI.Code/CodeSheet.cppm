// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - :code_sheet partition
//
// The code editor's default sheet (round three's ruling 4): its gutter, caret, selection and each
// token's colour through tokens over the theme's (`--code-keyword`, `--code-gutter`), dark by
// default and light where the scheme is, so a theme or an application recolours code by setting
// them over this sheet, not a fork of it. The font is the application's (a monospace family).

module;
#include "Core/Prelude.h"

export module experimental.gui.code:code_sheet;

import foundation.core;

namespace core = foundation::core;

export namespace experimental::gui
{
    /// The code editor's default sheet, written against the theme's tokens (an application puts it
    /// after the theme in its user agent sheet).
    [[nodiscard]] inline core::StringView CodeSheetCSS() noexcept
    {
        return core::StringView(u8R"(
            :root {
                --code-background: #16191f; --code-text: #d4d9e1; --code-gutter: #16191f; --code-line-number: #5c6573;
                --code-current-line-number: #c9d0da; --code-current-line: #1c2028;
                --code-selection: rgba(64, 120, 210, 0.42); --code-caret: #e8ecf2;
                --code-keyword: #c792ea; --code-type: #82aaff; --code-constant: #f78c6c; --code-builtin: #89ddff;
                --code-number: #f78c6c; --code-string: #c3e88d; --code-escape: #89ddff; --code-comment: #6b7586;
                --code-preprocessor: #ffcb6b; --code-operator: #89ddff; --code-punctuation: #a6aebb;
                --code-tag: #f07178; --code-attribute: #ffcb6b; --code-property: #82aaff; --code-selector: #c792ea;
                --code-invalid: #ff5370; --code-bracket-match: #2c3442;
            }
            @media (prefers-color-scheme: light) {
                :root {
                    --code-background: #fbfcfd; --code-text: #24292f; --code-gutter: #f3f5f8; --code-line-number: #9aa3ae;
                    --code-current-line-number: #24292f; --code-current-line: #f0f2f5;
                    --code-selection: rgba(80, 140, 230, 0.28); --code-caret: #24292f;
                    --code-keyword: #8250df; --code-type: #0550ae; --code-constant: #cf222e; --code-builtin: #0a7ea4;
                    --code-number: #b35900; --code-string: #0a6e31; --code-escape: #0a7ea4; --code-comment: #6e7781;
                    --code-preprocessor: #953800; --code-operator: #0a7ea4; --code-punctuation: #57606a;
                    --code-tag: #116329; --code-attribute: #953800; --code-property: #0550ae; --code-selector: #8250df;
                    --code-invalid: #cf222e; --code-bracket-match: #dde6f2;
                }
            }
            codeeditor { background-color: var(--code-background); color: var(--code-text); padding: 4px 0; }
            codeeditor:focus-visible { box-shadow: inset 0 0 0 var(--focus-width) var(--focus-ring); }
            codeeditor::gutter { background-color: var(--code-gutter); }
            codeeditor::line-number { color: var(--code-line-number); }
            codeeditor::current-line-number { color: var(--code-current-line-number); }
            codeeditor::current-line { background-color: var(--code-current-line); }
            codeeditor::selection { background-color: var(--code-selection); }
            codeeditor::caret { background-color: var(--code-caret); }
            codeeditor::bracket-match { background-color: var(--code-bracket-match); }
            codeeditor::keyword { color: var(--code-keyword); }
            codeeditor::type { color: var(--code-type); }
            codeeditor::constant { color: var(--code-constant); }
            codeeditor::builtin { color: var(--code-builtin); }
            codeeditor::number { color: var(--code-number); }
            codeeditor::string { color: var(--code-string); }
            codeeditor::escape { color: var(--code-escape); }
            codeeditor::comment { color: var(--code-comment); }
            codeeditor::preprocessor { color: var(--code-preprocessor); }
            codeeditor::operator { color: var(--code-operator); }
            codeeditor::punctuation { color: var(--code-punctuation); }
            codeeditor::tag { color: var(--code-tag); }
            codeeditor::attribute { color: var(--code-attribute); }
            codeeditor::property { color: var(--code-property); }
            codeeditor::selector { color: var(--code-selector); }
            codeeditor::invalid { color: var(--code-invalid); }
        )");
    }
}
