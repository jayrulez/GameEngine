// Draconic Runtime - :plugin partition
//
// IRuntimePlugin: a unit of engine functionality delivered separately from the
// core executable. A plugin registers its subsystems/services into the Context
// on load and removes them on unload. Plugins may be linked statically (handed
// to PluginHost::Add) or loaded from a shared library at runtime
// (PluginHost::Load). Plugins own the subsystems they register - they register
// them non-owningly via Context::RegisterSubsystem and tear them down in
// OnUnload, before the backing library is closed.

module;
#include "Draconic.Foundation/Prelude.h"

export module draconic.runtime:plugin;

import draconic.foundation;
import :context;

namespace foundation = draconic::foundation;

export namespace draconic::runtime
{
    class IRuntimePlugin
    {
    public:
        virtual ~IRuntimePlugin() = default;

        // Human-readable identifier (for logging/diagnostics).
        [[nodiscard]] virtual foundation::StringView Name() const noexcept = 0;

        // Register the plugin's subsystems/services into the Context.
        virtual void OnLoad(Context& context) = 0;

        // Remove what OnLoad added. Called before the backing library is closed,
        // so it is safe to touch plugin-defined types and destroy subsystems here.
        virtual void OnUnload(Context& context) = 0;
    };

    // Signature of the factory a dynamically-loaded plugin library must export
    // under the C name in CreatePluginSymbol. It returns a plugin instance owned
    // by the library (typically a function-local static); the host never frees
    // it - it closes the library after OnUnload.
    using CreatePluginFn = IRuntimePlugin* (*)();

    // The exported symbol name PluginHost::Load resolves in a plugin library.
    inline constexpr foundation::StringView CreatePluginSymbol = u8"DraconicCreatePlugin";
}
