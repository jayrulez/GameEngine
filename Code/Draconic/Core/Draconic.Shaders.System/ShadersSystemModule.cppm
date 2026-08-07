/// Draconic::ShaderSystem - the `draconic.shaders.system` module.
///
/// The variant compile-on-demand cache (:shader_system) + the dev file-backed
/// source provider (:file_provider) that serves engine built-in shaders from the
/// engine shader root (shaders.md P1).

export module draconic.shaders.system;

export import :shader_system;
export import :file_provider;
export import :host;
