// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_sheet_loader partition
//
// Entry point for loading .sss stylesheet files. Images are borrowed pointers; the loader does not own
// them. InitializeGlobals registers the drawable factories and the built-in UI type names.

module;
#include "Core/Prelude.h"

export module foundation.ui:style_sheet_loader;

import foundation.core;
import foundation.image;
import :style_sheet;
import :style_rule;
import :style_value;
import :theme_palette;
import :sss_token;
import :sss_tokenizer;
import :sss_parser;
import :ui_type_registry;
import :iresource_provider;

using namespace foundation::core;
namespace image = foundation::image;

export namespace foundation::ui
{
    class StyleSheetLoader
    {
    public:
        // The owner's allocator (required) backs every sheet, rule and drawable the loader
        // parses.
        explicit StyleSheetLoader(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        /// Resource provider for @import, @icon file loading and the image() factory (non-owning).
        IResourceProvider* ResourceProvider = nullptr;

        /// Register an SVG by name from inline text. Available to svg(name) in .sss.
        void RegisterSvg(StringView name, StringView svgText)
        {
            m_svgRegistry.InsertOrAssign(String(name), String(svgText));
        }

        /// Register an image by name from pre-built data. Available to image(name) in .sss. The loader
        /// does not own the image data; the caller is responsible for its lifetime.
        void RegisterImage(StringView name, const image::ImageData* imageData)
        {
            m_imageRegistry.InsertOrAssign(String(name), imageData);
        }

        /// Set a base palette variable. .sss @palette blocks can override these.
        void SetPaletteVariable(StringView name, Color color)
        {
            m_basePalette.InsertOrAssign(String(name), color);
        }

        /// Set base palette from a ThemePalette struct.
        void SetPalette(ThemePalette p)
        {
            SetPaletteVariable(u8"primary", p.Primary);
            SetPaletteVariable(u8"primary-accent", p.PrimaryAccent);
            SetPaletteVariable(u8"background", p.Background);
            SetPaletteVariable(u8"surface", p.Surface);
            SetPaletteVariable(u8"surface-bright", p.SurfaceBright);
            SetPaletteVariable(u8"border", p.Border);
            SetPaletteVariable(u8"text", p.Text);
            SetPaletteVariable(u8"text-dim", p.TextDim);
            SetPaletteVariable(u8"error", p.Error);
            SetPaletteVariable(u8"success", p.Success);
            SetPaletteVariable(u8"warning", p.Warning);
        }

        /// Load a StyleSheet from .sss text content.
        RefPtr<StyleSheet> Load(StringView source, StringView basePath = {})
        {
            // Registration is idempotent, so Load always ensures it. A host that skipped the
            // InitializeGlobals startup call would otherwise get type selectors that resolve to
            // null and match nothing (and the parser would eat the first declaration during
            // recovery).
            InitializeGlobals();

            // Copy palette so the parser can mutate it without affecting the loader.
            HashMap<String, Color> palette;
            for (const auto& kv : m_basePalette)
            {
                palette.InsertOrAssign(kv.key, kv.value);
            }

            Tokenizer tokenizer(source);
            Array<Token> tokens;
            tokenizer.TokenizeAll(tokens);

            SSSParser parser(*m_allocator, Move(tokens), &palette, &m_svgRegistry,
                             &m_imageRegistry, ResourceProvider, String(basePath));
            RefPtr<StyleSheet> sheet = parser.Parse();

            // The palette (the base and the sheet's @palette blocks) also becomes a root rule of
            // custom properties, so `$primary` at parse time and `var(--primary)` or
            // View::CustomProperty(u8"--primary") at compute time read the same colors. The rule
            // is prepended, so any `--name` the sheet declares on View wins over it.
            if (!palette.IsEmpty())
            {
                RefPtr<StyleRule> variables = MakeRef<StyleRule>(*m_allocator);
                variables->Selector.ViewType = UITypeRegistry::Resolve(u8"View");
                for (const auto& kv : palette)
                {
                    String name(u8"--");
                    name.Append(kv.key.AsView());
                    variables->SetCustom(name.AsView(), StyleValue::ColorVal(kv.value));
                }
                sheet->PrependRule(Move(variables));
            }
            return sheet;
        }

        /// Convenience: initialize registries (idempotent). Call once at startup.
        static void InitializeGlobals()
        {
            DrawableFactoryRegistry::RegisterBuiltins();
            UITypeRegistry::RegisterBuiltins();
        }

    private:
        IAllocator* m_allocator;
        HashMap<String, String> m_svgRegistry;
        HashMap<String, const image::ImageData*> m_imageRegistry;
        HashMap<String, Color> m_basePalette;
    };
}
