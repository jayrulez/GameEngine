// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Json - `foundation.json` (hand-rolled JSON DOM: value, parser and writer, UTF-8).
//
// The engine's boundary codec for JSON: the MCP wire protocol and game-chosen data interchange.
// It is not an engine data format (no ISerializer backend; engine data stays XML). The value type
// is RTTI-reflected (see :reflection) so scripts can parse, build, query and stringify JSON
// through the standard bound-object machinery. Value semantics throughout: no interior pointers
// escape.

export module foundation.json;

export import :value;
export import :writer;
export import :parser;
export import :reflection;

// The two convenience members declared on JsonValue (ToString, Parse) are defined in the
// implementation unit (ReflectionImpl.cpp), not here: defined inline in this primary interface
// unit, MSVC emits no body for the address-taken forms (&JsonValue::ToString) and consumers fail
// with LNK2019. They need :writer and :parser to define, which the implementation unit sees.
