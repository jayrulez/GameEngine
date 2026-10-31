# Foundation::Shaders.Resource - `foundation.shaders.resource` (shaders as resources: ShaderSource
# to ShaderResource through ShaderFactory; ties into the resource manager and ShaderSystem).
util_add_engine_library(Shaders.Resource ALIAS Foundation::Shaders.Resource)
target_link_libraries(Shaders.Resource
    PUBLIC Foundation::Core Foundation::RHI Foundation::Shaders Foundation::Shaders.System
           Foundation::Resource Foundation::Content Foundation::Policy)

target_sources(Shaders.Resource
    PUBLIC
        FILE_SET shader_resource_modules TYPE CXX_MODULES FILES
            ShaderResource.cppm)

