// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Xml - the `foundation.xml` module.
//
// A DOM XML parser and writer (UTF-8), the base for UI markup, SVG and the XML
// serialization backend. One named module composed of partitions.

export module foundation.xml;

export import :result;
export import :lexer;
export import :escape;
export import :ns;
export import :nodes;
export import :writer;
export import :document;
