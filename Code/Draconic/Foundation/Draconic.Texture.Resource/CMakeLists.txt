# Draconic::Texture.Resource - `draconic.texture.resource` (runtime cooked record + GPU factory).
add_library(Draconic.Texture.Resource STATIC)
add_library(Draconic::Texture.Resource ALIAS Draconic.Texture.Resource)
target_link_libraries(Draconic.Texture.Resource PUBLIC Draconic::Foundation Draconic::RHI Draconic::Texture Draconic::Content Draconic::Resource Draconic::Policy)
target_sources(Draconic.Texture.Resource PUBLIC FILE_SET texture_resource_modules TYPE CXX_MODULES FILES TextureResource.cppm)

