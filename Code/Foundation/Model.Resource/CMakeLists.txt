# Foundation::Model.Resource - `foundation.model.resource` (cooked model manifest, runtime composite and factory).
util_add_engine_library(Model.Resource ALIAS Foundation::Model.Resource)
target_link_libraries(Model.Resource PUBLIC
    Foundation::Core Foundation::Geometry Foundation::Geometry.Resource Foundation::Materials Foundation::Materials.Resource
    Foundation::Texture Foundation::Texture.Resource Foundation::Animation Foundation::Animation.Resource
    Foundation::Resource Foundation::Content Foundation::Policy)
target_sources(Model.Resource PUBLIC FILE_SET model_resource_modules TYPE CXX_MODULES FILES ModelResource.cppm)
