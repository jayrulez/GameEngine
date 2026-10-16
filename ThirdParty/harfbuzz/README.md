# HarfBuzz (vendored)

Source: https://github.com/harfbuzz/harfbuzz
Release: 14.5.1 (2026-09-30), harfbuzz-14.5.1.tar.xz,
sha256 7e2fa4e8c7c98e8d8140671f5772542afaaa6acccfbd746506886b6d85f7f8d6
License: the "Old MIT" license (COPYING). The Universal Shaping Engine tables are generated in part
from Microsoft data under the MIT license (src/ms-use/COPYING).

Trimmed vendoring (the meshoptimizer precedent): the library's own amalgamation, src/harfbuzz.cc,
the 68 sources it includes, every public and internal header, and src/OT/. Left out: the subsetter
(harfbuzz-subset.cc, graph/, hb-subset*.cc), the raster, vector, GPU, Cairo and world builds, the
Rust and wasm shapers, the generator scripts, the ragel inputs and the Unicode data text files
(their generated tables are kept), tests, fuzzers, docs and the util front ends.

Built with no FreeType, glib, ICU, Graphite or platform shaper: its built-in Unicode data and
OpenType shaping. Used only behind the engine's own ITextShaper (Fonts.HarfBuzz); no HarfBuzz type
or header crosses that interface (Specs/vg-and-gui-lab.md, F3).
