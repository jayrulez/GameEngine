// SPDX-License-Identifier: MIT
// Copyright (c) 2026-Present Robert Campbell

// Foundation::Input - the `foundation.input` module.
//
// The engine-level ACTION layer: named actions over data-driven
// bindings, evaluated against the shell's device facades. Engine-global, not per-scene -
// input belongs to a player, not a world. The raw device layer lives in foundation.shell;
// the asset/cooked forms live in foundation.input.editor / foundation.input.resource; the
// runtime hookup lives in engine.input.

export module foundation.input;

export import :input_map;
export import :action_runtime;
export import :binding_names;
export import :scripted_input;
export import :scripted_input_json;
export import :input_kind;
