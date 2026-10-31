# Foundation::Script.Resource - `foundation.script.resource` (cooked script classes and their factory).
util_add_engine_library(Script.Resource ALIAS Foundation::Script.Resource)
target_link_libraries(Script.Resource PUBLIC
    Foundation::Core Foundation::Resource Foundation::Content Foundation::Policy)
target_sources(Script.Resource PUBLIC FILE_SET script_resource_modules TYPE CXX_MODULES FILES ScriptResource.cppm)

