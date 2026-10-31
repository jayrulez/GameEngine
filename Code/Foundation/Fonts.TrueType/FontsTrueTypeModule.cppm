// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Fonts.TrueType - the `foundation.fonts.truetype` module.
//
// TrueType/OpenType backend over stb_truetype: a TrueTypeFont (IFont), a
// stb-packed TrueTypeFontAtlas (IFontAtlas), the matching parser and atlas baker,
// a text shaper, the registration helper (TrueTypeFonts), and a VFS-aware
// IFontService. One named module composed of partitions.

export module foundation.fonts.truetype;

export import :common;
export import :font;
export import :atlas;
export import :parser;
export import :atlas_baker;
export import :text_shaper;
export import :init;
export import :service;
