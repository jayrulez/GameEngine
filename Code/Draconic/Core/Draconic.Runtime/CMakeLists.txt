# Draconic::Runtime - the `draconic.runtime` module (Context + Subsystems).

add_library(Draconic.Runtime STATIC)
add_library(Draconic::Runtime ALIAS Draconic.Runtime)

target_link_libraries(Draconic.Runtime PUBLIC Draconic::Foundation Draconic::Policy)

target_sources(Draconic.Runtime
    PUBLIC
        FILE_SET runtime_modules TYPE CXX_MODULES FILES
            RuntimeModule.cppm
            Subsystem.cppm
            RuntimeContext.cppm
            Plugin.cppm
            PluginHost.cppm)
