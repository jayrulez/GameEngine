# Draconic::Resource - `draconic.resource` (resource manager: source->product via
# factories, replaceable handles + proxies, caching + hot reload over content).

add_library(Draconic.Resource STATIC)
add_library(Draconic::Resource ALIAS Draconic.Resource)

target_link_libraries(Draconic.Resource PUBLIC Draconic::Foundation Draconic::Content Draconic::Policy)

target_sources(Draconic.Resource
    PUBLIC
        FILE_SET resource_modules TYPE CXX_MODULES FILES
            ResourceModule.cppm)

