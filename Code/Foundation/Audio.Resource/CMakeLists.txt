# Foundation::Audio.Resource - `foundation.audio.resource` (cooked audio clips and their factory).
util_add_engine_library(Audio.Resource ALIAS Foundation::Audio.Resource)
target_link_libraries(Audio.Resource PUBLIC
    Foundation::Core Foundation::Resource Foundation::Content Foundation::Audio Foundation::Policy)
target_sources(Audio.Resource PUBLIC FILE_SET audio_resource_modules TYPE CXX_MODULES FILES AudioResource.cppm)
