# Foundation::Texture.Resource - `foundation.texture.resource` (runtime cooked record and GPU factory).
util_add_engine_library(Texture.Resource ALIAS Foundation::Texture.Resource)
target_link_libraries(Texture.Resource PUBLIC Foundation::Core Foundation::RHI Foundation::Texture Foundation::Content Foundation::Resource Foundation::Policy)
target_sources(Texture.Resource PUBLIC FILE_SET texture_resource_modules TYPE CXX_MODULES FILES TextureResource.cppm)

