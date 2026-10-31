// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts - the `foundation.fonts` module.
//
// Font/glyph/text-layout types and abstract interfaces (IFont, IFontAtlas,
// ITextShaper, IFontService) plus shared helpers. Backends (TTF, baked) and the
// IO layer build on these. One named module composed of partitions.

export module foundation.fonts;

export import :types;
export import :interfaces;
export import :text_util;
export import :atlas_texture;
export import :null_service;
export import :scaled_views;
export import :backend_interfaces; // parser, baker and cache interfaces
export import :factories;
export import :font_manager;
export import :bidi;
export import :cluster_text_shaper;
