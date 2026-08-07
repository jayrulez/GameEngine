// Draconic::Fonts - the `draconic.fonts` module.
//
// Font/glyph/text-layout types and abstract interfaces (IFont, IFontAtlas,
// ITextShaper, IFontService) plus shared helpers. Backends (TTF, baked) and the
// IO layer build on these. Ported from Sedulous.Fonts (excluding the
// resource-database integration). One named module composed of partitions.

export module draconic.fonts;

export import :types;
export import :interfaces;
export import :text_util;
export import :atlas_texture;
export import :null_service;
export import :scaled_views;
