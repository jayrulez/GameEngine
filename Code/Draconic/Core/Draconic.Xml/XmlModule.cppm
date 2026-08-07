// Draconic::Xml - the `draconic.xml` module.
//
// A DOM XML parser + writer (UTF-8), faithfully ported from the hand-written
// Sedulous.Xml library so future Sedulous ports (UI markup, SVG) and an XML
// serialization backend can build on a compatible API. One named module
// composed of partitions.

export module draconic.xml;

export import :result;
export import :lexer;
export import :escape;
export import :ns;
export import :nodes;
export import :writer;
export import :document;
