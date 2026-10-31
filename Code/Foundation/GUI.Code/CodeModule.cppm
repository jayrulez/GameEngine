// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// GUI.Code - `foundation.gui.code`: the code editor, a library beside the GUI. It knows no
// language; the owner of a language supplies its definition and its service.

module;
#include "Core/Prelude.h"

export module foundation.gui.code;

export import :code_document; // lines, positions, anchors, edits, undo with selections
export import :code_language; // a language's definition (supplied by its owner) and its tokenizer
export import :code_sheet;    // the default sheet, over the theme's tokens
export import :code_find_bar; // the find, replace and go-to-line bar
export import :code_completion; // the language service (the host's) and the completion list
export import :code_editor;   // the view: shaped lines in view, several carets, the keymap's text commands

export namespace foundation::gui
{
    /// Registers the code editor's element types (ElementTypes::Register) so markup and scripts can
    /// make them. A host calls it once at start-up, beside RegisterGuiElements.
    void RegisterCodeElements();
}
