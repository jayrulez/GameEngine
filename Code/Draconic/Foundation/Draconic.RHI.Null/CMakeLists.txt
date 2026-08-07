# Draconic::RHI.Null - `draconic.rhi.null` (headless no-op RHI backend, for tests/tools).

add_library(Draconic.RHI.Null STATIC)
add_library(Draconic::RHI.Null ALIAS Draconic.RHI.Null)

target_link_libraries(Draconic.RHI.Null PUBLIC Draconic::RHI Draconic::Foundation Draconic::Policy)

target_sources(Draconic.RHI.Null
    PUBLIC
        FILE_SET rhi_null_modules TYPE CXX_MODULES FILES
            NullModule.cppm)

