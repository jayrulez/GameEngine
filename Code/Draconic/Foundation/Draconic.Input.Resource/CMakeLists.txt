# Draconic::Input.Resource - `draconic.input.resource`: cooked input map + factory.
add_library(Draconic.Input.Resource STATIC)
add_library(Draconic::Input.Resource ALIAS Draconic.Input.Resource)

target_link_libraries(Draconic.Input.Resource
    PUBLIC Draconic::Foundation Draconic::Content Draconic::Resource Draconic::Input Draconic::Policy)

target_sources(Draconic.Input.Resource
    PUBLIC
        FILE_SET input_resource_modules TYPE CXX_MODULES FILES
            InputResource.cppm)
