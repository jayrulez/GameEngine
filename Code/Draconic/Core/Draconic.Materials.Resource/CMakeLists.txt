# Draconic::Materials.Resource - `draconic.materials.resource` (materials as resources:
# MaterialSource -> Material via MaterialFactory; depends on the ShaderResource).
add_library(Draconic.Materials.Resource STATIC)
add_library(Draconic::Materials.Resource ALIAS Draconic.Materials.Resource)

target_link_libraries(Draconic.Materials.Resource
    PUBLIC Draconic::Foundation Draconic::Resource Draconic::Content Draconic::Shaders
           Draconic::Texture Draconic::Texture.Resource
           Draconic::Shaders.Resource Draconic::Materials Draconic::Policy)

target_sources(Draconic.Materials.Resource
    PRIVATE
        MaterialResourceImpl.cpp   # MaterialSource reflection body (GCC hygiene: out of the interface)
    PUBLIC
        FILE_SET material_resource_modules TYPE CXX_MODULES FILES
            MaterialResource.cppm)

