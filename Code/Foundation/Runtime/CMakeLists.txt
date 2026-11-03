# Foundation::Runtime - the `foundation.runtime` module (SubsystemManager and Subsystems, the
# plugin contract and host, and the plugin manifest, which is XML: hence Xml.Serialization).

util_add_engine_library(Runtime ALIAS Foundation::Runtime)
target_link_libraries(Runtime PUBLIC Foundation::Core Foundation::Run Foundation::Xml.Serialization
    Foundation::Policy)

target_sources(Runtime
    PUBLIC
        FILE_SET runtime_modules TYPE CXX_MODULES FILES
            RuntimeModule.cppm
            Subsystem.cppm
            SubsystemManager.cppm
            Plugin.cppm
            PluginHost.cppm
            PluginManifest.cppm)
