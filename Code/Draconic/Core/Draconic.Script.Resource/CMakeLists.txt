# Draconic::Script.Resource - `draconic.script.resource` (cooked script classes + factory).
add_library(Draconic.Script.Resource STATIC)
add_library(Draconic::Script.Resource ALIAS Draconic.Script.Resource)
target_link_libraries(Draconic.Script.Resource PUBLIC
    Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Policy)
target_sources(Draconic.Script.Resource PUBLIC FILE_SET script_resource_modules TYPE CXX_MODULES FILES ScriptResource.cppm)

