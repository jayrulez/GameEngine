# SheenBidi (vendored)

Source: https://github.com/Tehreer/SheenBidi
Release: v3.0.0 (2026-01-24), the v3.0.0 tag archive,
sha256 86c56014034739ba39a24c23eb00323b0bf6f737354f665786015fca842af786
License: Apache-2.0 (LICENSE). The project ships no NOTICE file.

Vendored: Headers/ and Source/ as released (the library and its Unicode data tables), built from its
own unity source (Source/SheenBidi.c with SB_CONFIG_UNITY) as C. Left out: the tests, the tools that
generate the tables, and the build files (meson, make, CMake, pkg-config).

Used only behind the engine's own bidi interface (Fonts.Bidi): no SheenBidi type or header crosses
it (Specs/vg-and-gui-lab.md, F3).
