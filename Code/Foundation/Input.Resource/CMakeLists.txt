# Foundation::Input.Resource - `foundation.input.resource`: cooked input map and its factory.
util_add_engine_library(Input.Resource ALIAS Foundation::Input.Resource)
target_link_libraries(Input.Resource
    PUBLIC Foundation::Core Foundation::Content Foundation::Resource Foundation::Input Foundation::Policy)

target_sources(Input.Resource
    PUBLIC
        FILE_SET input_resource_modules TYPE CXX_MODULES FILES
            InputResource.cppm)
