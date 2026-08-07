# Draconic::Image.Resource - `draconic.image.resource` (runtime CPU-image resource).
add_library(Draconic.Image.Resource STATIC)
add_library(Draconic::Image.Resource ALIAS Draconic.Image.Resource)
target_link_libraries(Draconic.Image.Resource PUBLIC Draconic::Foundation Draconic::Image Draconic::Content Draconic::Resource Draconic::Policy)
target_sources(Draconic.Image.Resource PUBLIC FILE_SET image_resource_modules TYPE CXX_MODULES FILES ImageResource.cppm)
