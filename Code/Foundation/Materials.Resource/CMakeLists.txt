# Foundation::Materials.Resource - `foundation.materials.resource` (materials as resources:
# MaterialSource to Material via MaterialFactory; depends on the ShaderResource).
util_add_engine_library(Materials.Resource ALIAS Foundation::Materials.Resource)
target_link_libraries(Materials.Resource
    PUBLIC Foundation::Core Foundation::Resource Foundation::Content Foundation::Shaders
           Foundation::Texture Foundation::Texture.Resource
           Foundation::Shaders.Resource Foundation::Materials Foundation::Policy)

target_sources(Materials.Resource
    PRIVATE
        MaterialResourceImpl.cpp   # MaterialSource reflection body (kept out of the interface for GCC)
    PUBLIC
        FILE_SET material_resource_modules TYPE CXX_MODULES FILES
            MaterialResource.cppm)

