# Foundation::Runtime - the `foundation.runtime` module (SubsystemManager and Subsystems).

util_add_engine_library(Runtime ALIAS Foundation::Runtime)
target_link_libraries(Runtime PUBLIC Foundation::Core Foundation::Run Foundation::Policy)

target_sources(Runtime
    PUBLIC
        FILE_SET runtime_modules TYPE CXX_MODULES FILES
            RuntimeModule.cppm
            Subsystem.cppm
            SubsystemManager.cppm
            Plugin.cppm
            PluginHost.cppm)
