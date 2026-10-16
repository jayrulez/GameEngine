// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Packing by glyph index. stb_truetype's pack API names glyphs by codepoint only, and a shaper's
// output (ligatures, joined forms) has glyphs no codepoint reaches, so atlases pack glyph indices
// through this instead. It gathers, packs, oversamples and prefilters exactly as
// stbtt_PackFontRanges does, so a glyph packs the same whichever way it is named. Implemented in
// stb_truetype_impl.cpp, where stb's rect type is complete.
#pragma once

#include "stb_truetype.h"

// Pack `count` glyphs of `info` at `pixelHeight` into `spc`, writing `out[i]` for `glyphs[i]`.
// Returns 1 when every glyph packed, 0 when the atlas ran out of room (or memory).
int EngineStbPackGlyphs(stbtt_pack_context* spc, const stbtt_fontinfo* info, float pixelHeight, const int* glyphs,
                        int count, stbtt_packedchar* out);
