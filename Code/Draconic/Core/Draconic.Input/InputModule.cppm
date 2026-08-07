// Draconic::Input - the `draconic.input` module.
//
// The engine-level ACTION layer (docs/design/input.md): named actions over data-driven
// bindings, evaluated against the shell's device facades. Engine-global, not per-scene -
// input belongs to a player, not a world. The raw device layer lives in draconic.shell;
// the asset/cooked forms live in draconic.input.editor / draconic.input.resource; the
// runtime hookup lives in draconic.engine.input.

export module draconic.input;

export import :input_map;
export import :action_runtime;
