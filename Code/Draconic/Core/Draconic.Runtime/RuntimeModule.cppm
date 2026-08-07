// Draconic::Runtime - the `draconic.runtime` module.
//
// The engine runtime: a Context owns Subsystems and drives their lifecycle and
// per-frame phases. The application loop and plugin host build on this.

export module draconic.runtime;

export import :subsystem;
export import :context;
export import :plugin;
export import :pluginhost;
