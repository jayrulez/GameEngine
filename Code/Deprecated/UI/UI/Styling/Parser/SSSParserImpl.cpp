// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - module implementation unit for SSSParser::ApplyInlineStyle.
//
// Holds the View-touching body of SSSParser::ApplyInlineStyle (it reaches into the View cluster for
// GetOrCreateInlineSheet and Invalidate), so :sss_parser stays free of a :view import. Inline-style
// parsing uses empty palette and asset registries, so $variables, @icon and @image references are not
// resolved.

module;
#include "Core/Prelude.h"

module foundation.ui;

import foundation.image; // image::ImageData, for the empty image registry

using namespace foundation::core;
namespace image = foundation::image;

namespace foundation::ui
{
    void SSSParser::ApplyInlineStyle(View* view, StringView body)
    {
        // Ensure the drawable factories (rounded-rect, gradient, state-*, svg, ...) are registered;
        // otherwise an inline value like `background: rounded-rect(...)` is not recognized and falls
        // back to a plain white color. StyleSheetLoader does this for .sss files; the inline path must too.
        DrawableFactoryRegistry::RegisterBuiltins();

        Array<Token> tokens;
        Tokenizer tokenizer(body);
        tokenizer.TokenizeAll(tokens);

        // Empty palette and asset registries, held here so they outlive the parser, which keeps raw
        // pointers to them.
        HashMap<String, Color> palette;
        HashMap<String, String> svg;
        HashMap<String, const image::ImageData*> img;
        String basePath;

        SSSParser parser(view->MemoryAllocator(), Move(tokens), &palette, &svg, &img, nullptr,
                         Move(basePath));

        // Point the parser at the view's inline sheet so any drawable values live on the view (the
        // inline sheet owns them).
        StyleSheet& inlineSheet = view->GetOrCreateInlineSheet();
        StyleRule& rule = inlineSheet.GetOrCreateInlineElementRule();
        parser.ParseDeclarations(&inlineSheet, rule);

        view->Invalidate();
    }
}
