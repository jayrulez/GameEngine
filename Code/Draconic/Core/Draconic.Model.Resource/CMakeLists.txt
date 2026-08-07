# Draconic::Model.Resource - `draconic.model.resource` (cooked model manifest + runtime composite + factory).
add_library(Draconic.Model.Resource STATIC)
add_library(Draconic::Model.Resource ALIAS Draconic.Model.Resource)
target_link_libraries(Draconic.Model.Resource PUBLIC
    Draconic::Foundation Draconic::Geometry Draconic::Geometry.Resource Draconic::Materials Draconic::Materials.Resource
    Draconic::Texture Draconic::Texture.Resource Draconic::Animation Draconic::Animation.Resource
    Draconic::Resource Draconic::Content Draconic::Policy)
target_sources(Draconic.Model.Resource PUBLIC FILE_SET model_resource_modules TYPE CXX_MODULES FILES ModelResource.cppm)
