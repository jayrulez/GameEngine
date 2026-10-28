// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// UI - :style_sheet_loader partition
//
// Entry point for loading .sss stylesheet files. Ported from
// Sedulous.UI/src/Styling/Parser/StyleSheetLoader.bf.
//
// Divergences (language): Beef Dictionaries with manual key/value deletes -> HashMap (RAII); Load
// returns RefPtr<StyleSheet>; images are borrowed pointers (loader does not own them). InitializeGlobals
// registers the drawable factories; UITypeRegistry::RegisterBuiltins is DEFERRED until the control
// classes land, so it is not called yet.

module;
#include "Core/Prelude.h"

export module foundation.ui:style_sheet_loader;

import foundation.core;  // HashMap, String, StringView, Color, RefPtr
import foundation.image; // ImageData
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
        // The allocator (required - the owner decides) backs every sheet/rule/drawable
        // the loader parses.
        explicit StyleSheetLoader(IAllocator& allocator) noexcept : m_allocator(&allocator) {}

        /// Resource provider for @import, @icon file loading, image() factory (non-owning).
        IResourceProvider* ResourceProvider = nullptr;

        /// Register an SVG by name from inline text. Available to svg(name) in .sss.
        void RegisterSvg(StringView name, StringView svgText)
        {
            m_svgRegistry.InsertOrAssign(String(name), String(svgText));
        }

        /// Register an image by name from pre-built data. Available to image(name) in .sss. The loader
        /// does NOT own the image data - caller is responsible for lifetime.
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
            // Self-sufficient: hosts that skip the
            // InitializeGlobals startup call would get TYPE selectors that silently resolve
            // to null and match nothing (the parser then eats the first declaration during
            // recovery). Registration is idempotent - always ensure it here.
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

            // The palette (base + the sheet's @palette blocks) ALSO becomes a root rule of
            // custom properties - `$primary` at parse time and `var(--primary)` /
            // View::CustomProperty(u8"--primary") at compute time read the same colors.
            // Prepended, so any `--name` the sheet declares on View wins over it.
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
