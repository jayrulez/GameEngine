// Draconic::FontsIO - the `draconic.fonts.io` module.
//
// The source-format font load pipeline: IFontParser/IFontAtlasBaker contracts,
// the extension-routed factories that dispatch over them, and a thread-safe
// FontManager cache. Backends (TTF) register parsers/bakers here. Ported from
// Sedulous.Fonts.IO - its own library, matching Sedulous. One named module
// composed of partitions.

export module draconic.fonts.io;

export import :interfaces;
export import :factories;
export import :manager;
