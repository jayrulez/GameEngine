# Draconic::Shaders.Resource - `draconic.shaders.resource` (shaders as resources: ShaderSource
# -> ShaderResource via ShaderFactory; ties into the resource manager + ShaderSystem).
add_library(Draconic.Shaders.Resource STATIC)
add_library(Draconic::Shaders.Resource ALIAS Draconic.Shaders.Resource)

target_link_libraries(Draconic.Shaders.Resource
    PUBLIC Draconic::Foundation Draconic::RHI Draconic::Shaders Draconic::Shaders.System
           Draconic::Resource Draconic::Content Draconic::Policy)

target_sources(Draconic.Shaders.Resource
    PUBLIC
        FILE_SET shader_resource_modules TYPE CXX_MODULES FILES
            ShaderResource.cppm)

